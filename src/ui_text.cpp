/*
Copyright (c) 2026 Jørgen Lind

  Permission is hereby granted, free of charge, to any person obtaining a copy of
  this software and associated documentation files (the "Software"), to deal in
  the Software without restriction, including without limitation the rights to
  use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
  of the Software, and to permit persons to whom the Software is furnished to do
  so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

#include "ui_text.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <unordered_map>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <hb-ot.h>
#include <hb.h>

namespace vui::ui
{
namespace detail
{
// What a font_t shares: the file's bytes and HarfBuzz's face over them. Both are immutable once
// made, and an hb_face_t is safe to use from any thread, which is what lets font_t be shared. A
// synthetic face (G75) shares the bytes and the hb_face_t of the face it is made from, and is told
// apart from it by its flags, which FreeType applies to each outline it loads.
struct font_data_t
{
  std::shared_ptr<const std::vector<std::byte>> bytes;
  hb_face_t *face = nullptr;
  unsigned index = 0;
  // Where it was loaded from, which is where its family's other faces are looked for.
  std::string path;
  font_style_t style = font_style_t::regular;
  bool embolden = false;
  bool oblique = false;

  // The family's faces by style, each looked for the first time it is asked for. Only the face
  // first asked holds them; the faces it made point back to it weakly, so nothing holds itself.
  std::weak_ptr<const font_data_t> family;
  mutable std::mutex mutex;
  mutable std::array<std::shared_ptr<const font_data_t>, 4> styles;
  mutable std::array<bool, 4> searched{};

  font_data_t() = default;
  font_data_t(const font_data_t &) = delete;
  font_data_t &operator=(const font_data_t &) = delete;
  ~font_data_t()
  {
    if (face)
      hb_face_destroy(face);
  }
};

namespace
{
std::uint16_t read_u16(const char *at) noexcept
{
  return std::uint16_t((std::uint8_t(at[0]) << 8) | std::uint8_t(at[1]));
}

// The style a face says it is: OS/2's selection bits and weight, and head's macStyle.
font_style_t style_of(hb_face_t *face)
{
  unsigned bits = 0;
  hb_blob_t *os2 = hb_face_reference_table(face, HB_TAG('O', 'S', '/', '2'));
  unsigned length = 0;
  const char *data = hb_blob_get_data(os2, &length);
  if (data && length >= 64)
  {
    const unsigned selection = read_u16(data + 62);
    if ((selection & 1u) != 0 || (selection & (1u << 9)) != 0)
      bits |= 2;
    if ((selection & (1u << 5)) != 0 || read_u16(data + 4) >= 600)
      bits |= 1;
  }
  hb_blob_destroy(os2);
  hb_blob_t *head = hb_face_reference_table(face, HB_TAG('h', 'e', 'a', 'd'));
  data = hb_blob_get_data(head, &length);
  if (data && length >= 46)
    bits |= read_u16(data + 44) & 3u;
  hb_blob_destroy(head);
  return font_style_t(bits);
}

std::expected<std::shared_ptr<font_data_t>, std::string> make_data(std::shared_ptr<const std::vector<std::byte>> bytes, unsigned index)
{
  if (!bytes || bytes->empty())
    return std::unexpected(std::string("empty font data"));
  auto data = std::make_shared<font_data_t>();
  data->bytes = std::move(bytes);
  data->index = index;
  auto *blob = hb_blob_create(reinterpret_cast<const char *>(data->bytes->data()), unsigned(data->bytes->size()), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
  const unsigned faces = hb_face_count(blob);
  data->face = hb_face_create(blob, index);
  hb_blob_destroy(blob);
  if (index >= std::max(faces, 1u) || hb_face_get_glyph_count(data->face) == 0)
    return std::unexpected(std::string(index > 0 ? "no such face in the collection" : "not a TrueType or OpenType font"));
  data->style = style_of(data->face);
  return data;
}

// A whole file in one read: a fallback CJK font is tens of megabytes, and reading it a character at
// a time through a stream iterator took seconds in a Debug build, on the loop thread.
std::expected<std::shared_ptr<const std::vector<std::byte>>, std::string> read_file(const std::string &path)
{
  std::ifstream file(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary | std::ios::ate);
  if (!file)
    return std::unexpected("cannot open " + path);
  const auto size = static_cast<std::streamoff>(file.tellg());
  if (size < 0)
    return std::unexpected("cannot read " + path);
  auto bytes = std::make_shared<std::vector<std::byte>>(std::size_t(size));
  file.seekg(0);
  if (!file.read(reinterpret_cast<char *>(bytes->data()), size))
    return std::unexpected("cannot read " + path);
  return bytes;
}

std::filesystem::path to_path(const std::string &utf8)
{
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string to_utf8(const std::filesystem::path &path)
{
  const auto text = path.u8string();
  return std::string(text.begin(), text.end());
}

bool ends_with(const std::string &text, const std::string &suffix)
{
  if (suffix.size() > text.size())
    return false;
  return std::equal(suffix.rbegin(), suffix.rend(), text.rbegin(), [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
}

// Where a family keeps its faces beside the one loaded (G75). The stem is stripped of whatever style
// it names, in the long form (DejaVuSansMono-Bold, LiberationMono-Regular, CascadiaMonoItalic) and
// Windows' short one (consolab, courbd, segoeuiz), and every spelling of the style wanted is tried
// on what is left. What is found is checked by what the face says it is, so a guess that names some
// other face is never taken.
std::vector<std::string> sibling_paths(const std::string &path, font_style_t style)
{
  const std::filesystem::path file = to_path(path);
  const std::string stem = to_utf8(file.stem());
  const std::string extension = to_utf8(file.extension());
  std::vector<std::string> bases{stem};
  for (const std::string suffix : {"-Regular", "Regular", "-Roman", "-Book", "-BoldItalic", "-BoldOblique", "BoldItalic", "-Bold", "Bold", "-Italic", "-Oblique", "Italic", "Oblique"})
  {
    if (ends_with(stem, suffix) && stem.size() > suffix.size())
      bases.push_back(stem.substr(0, stem.size() - suffix.size()));
  }
  for (const std::string suffix : {"bi", "bd", "z", "b", "i"})
  {
    if (ends_with(stem, suffix) && stem.size() >= suffix.size() + 3)
      bases.push_back(stem.substr(0, stem.size() - suffix.size()));
  }
  std::vector<const char *> spellings;
  switch (style)
  {
  case font_style_t::regular:
    spellings = {"", "-Regular", "Regular", "-Roman", "-Book"};
    break;
  case font_style_t::bold:
    spellings = {"-Bold", "Bold", "b", "bd"};
    break;
  case font_style_t::italic:
    spellings = {"-Italic", "-Oblique", "Italic", "Oblique", "i", "-It"};
    break;
  case font_style_t::bold_italic:
    spellings = {"-BoldItalic", "-BoldOblique", "BoldItalic", "BoldOblique", "z", "bi", "-BoldIt"};
    break;
  }
  std::vector<std::string> out;
  for (const auto &base : bases)
  {
    for (const char *spelling : spellings)
    {
      auto candidate = to_utf8(file.parent_path() / to_path(base + spelling + extension));
      if (candidate != path && std::find(out.begin(), out.end(), candidate) == out.end())
        out.push_back(std::move(candidate));
    }
  }
  return out;
}

// The face of `style` in the same collection as `root`, or in the files beside it; null if there is
// none.
std::shared_ptr<const font_data_t> find_sibling(const font_data_t &root, font_style_t style)
{
  // Another face of the same collection first: Menlo.ttc holds all four.
  auto *blob = hb_blob_create(reinterpret_cast<const char *>(root.bytes->data()), unsigned(root.bytes->size()), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
  const unsigned faces = hb_face_count(blob);
  hb_blob_destroy(blob);
  for (unsigned i = 0; i < faces; ++i)
  {
    if (i == root.index)
      continue;
    auto data = make_data(root.bytes, i);
    if (data && (*data)->style == style)
    {
      (*data)->path = root.path;
      return std::move(*data);
    }
  }
  if (root.path.empty())
    return nullptr;
  for (const auto &candidate : sibling_paths(root.path, style))
  {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(to_path(candidate), ec))
      continue;
    auto bytes = read_file(candidate);
    if (!bytes)
      continue;
    auto data = make_data(std::move(*bytes), 0);
    if (data && (*data)->style == style)
    {
      (*data)->path = candidate;
      return std::move(*data);
    }
  }
  return nullptr;
}

// `from` drawn as `style` by FreeType: emboldened for a bold it lacks, slanted for an italic.
std::shared_ptr<const font_data_t> synthesize(const font_data_t &from, font_style_t style)
{
  auto data = std::make_shared<font_data_t>();
  data->bytes = from.bytes;
  data->face = hb_face_reference(from.face);
  data->index = from.index;
  data->path = from.path;
  data->style = style;
  const unsigned want = unsigned(style);
  const unsigned have = unsigned(from.style);
  data->embolden = from.embolden || ((want & 1u) != 0 && (have & 1u) == 0);
  data->oblique = from.oblique || ((want & 2u) != 0 && (have & 2u) == 0);
  return data;
}
} // namespace

// Decode the code point at `i` of UTF-8 `text` and step past it; a malformed byte is U+FFFD.
char32_t next_code_point(std::string_view text, std::size_t &i) noexcept
{
  const auto lead = static_cast<unsigned char>(text[i]);
  const int length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xe ? 3 : (lead >> 3) == 0x1e ? 4 : 0;
  if (length == 0 || i + std::size_t(length) > text.size())
  {
    ++i;
    return 0xfffd;
  }
  char32_t cp = length == 1 ? lead : char32_t(lead & (0x7f >> length));
  for (int k = 1; k < length; ++k)
  {
    const auto next = static_cast<unsigned char>(text[i + std::size_t(k)]);
    if ((next & 0xc0) != 0x80)
    {
      ++i;
      return 0xfffd;
    }
    cp = (cp << 6) | (next & 0x3f);
  }
  i += std::size_t(length);
  return cp;
}
} // namespace detail

// ---- font_t

std::expected<font_t, std::string> font_t::from_memory(std::vector<std::byte> bytes, unsigned index)
{
  auto data = detail::make_data(std::make_shared<const std::vector<std::byte>>(std::move(bytes)), index);
  if (!data)
    return std::unexpected(data.error());
  return font_t(std::move(*data));
}

std::expected<font_t, std::string> font_t::from_file(const std::string &path, unsigned index)
{
  auto bytes = detail::read_file(path);
  if (!bytes)
    return std::unexpected(bytes.error());
  auto data = detail::make_data(std::move(*bytes), index);
  if (!data)
    return std::unexpected(path + ": " + data.error());
  (*data)->path = path;
  return font_t(std::move(*data));
}

font_t font_t::styled(font_style_t style) const
{
  if (!_data || _data->style == style)
    return *this;
  // The family's faces are kept by the face first asked; a face made for it asks that one.
  std::shared_ptr<const detail::font_data_t> root = _data->family.lock();
  if (!root)
    root = _data;
  if (root->style == style)
    return font_t(root);
  std::lock_guard lock(root->mutex);
  // The family's own face of a style, or null; looked for once.
  const auto own = [&root](font_style_t of) -> std::shared_ptr<const detail::font_data_t>
  {
    const auto i = std::size_t(of);
    if (!root->searched[i])
    {
      root->searched[i] = true;
      if (auto found = detail::find_sibling(*root, of))
      {
        std::const_pointer_cast<detail::font_data_t>(found)->family = root;
        root->styles[i] = std::move(found);
      }
    }
    const auto &face = root->styles[i];
    return face && !face->embolden && !face->oblique ? face : nullptr;
  };
  auto &slot = root->styles[std::size_t(style)];
  if (!slot)
  {
    if (auto found = own(style))
      return font_t(found);
    // The nearest face the family has, made up the rest of the way: a bold italic from the italic,
    // or from the bold, where only one of them exists.
    std::shared_ptr<const detail::font_data_t> made;
    if (style == font_style_t::bold_italic)
    {
      for (const auto nearest : {font_style_t::italic, font_style_t::bold})
      {
        if (made || nearest == root->style)
          continue;
        if (auto face = own(nearest))
          made = detail::synthesize(*face, style);
      }
    }
    if (!made)
      made = detail::synthesize(*root, style);
    std::const_pointer_cast<detail::font_data_t>(made)->family = root;
    slot = std::move(made);
  }
  return font_t(slot);
}

font_style_t font_t::style() const noexcept
{
  return _data ? _data->style : font_style_t::regular;
}

bool font_t::synthetic() const noexcept
{
  return _data && (_data->embolden || _data->oblique);
}

bool font_t::has_glyph(char32_t codepoint) const
{
  if (!_data)
    return false;
  auto *hb = hb_font_create(_data->face);
  hb_codepoint_t glyph = 0;
  const bool found = hb_font_get_nominal_glyph(hb, codepoint, &glyph) && glyph != 0;
  hb_font_destroy(hb);
  return found;
}

std::expected<font_t, std::string> font_t::system_default()
{
  std::vector<std::string> candidates;
#if defined(_WIN32)
  const char *windir = std::getenv("WINDIR");
  const std::string fonts = std::string(windir ? windir : "C:\\Windows") + "\\Fonts\\";
  candidates = {fonts + "segoeui.ttf", fonts + "arial.ttf"};
#elif defined(__APPLE__)
  candidates = {"/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Helvetica.ttc", "/System/Library/Fonts/Geneva.ttf"};
#else
  for (const char *dir : {"/usr/share/fonts/truetype/", "/usr/share/fonts/TTF/", "/usr/share/fonts/", "/usr/local/share/fonts/"})
  {
    for (const char *name : {"dejavu/DejaVuSans.ttf", "DejaVuSans.ttf", "noto/NotoSans-Regular.ttf", "NotoSans-Regular.ttf", "google-noto/NotoSans-Regular.ttf", "liberation/LiberationSans-Regular.ttf",
                             "LiberationSans-Regular.ttf", "liberation-sans/LiberationSans-Regular.ttf"})
      candidates.push_back(std::string(dir) + name);
  }
#endif
  for (const auto &path : candidates)
  {
    if (auto font = from_file(path))
      return font;
  }
  return std::unexpected(std::string("no system UI font at any known path"));
}

std::expected<font_t, std::string> font_t::system_monospace()
{
  std::vector<std::string> candidates;
#if defined(_WIN32)
  const char *windir = std::getenv("WINDIR");
  const std::string fonts = std::string(windir ? windir : "C:\\Windows") + "\\Fonts\\";
  candidates = {fonts + "consola.ttf", fonts + "CascadiaMono.ttf", fonts + "lucon.ttf", fonts + "cour.ttf"};
#elif defined(__APPLE__)
  candidates = {"/System/Library/Fonts/Menlo.ttc", "/System/Library/Fonts/Monaco.ttf", "/System/Library/Fonts/SFNSMono.ttf", "/System/Library/Fonts/Courier.ttc"};
#else
  for (const char *dir : {"/usr/share/fonts/truetype/", "/usr/share/fonts/TTF/", "/usr/share/fonts/", "/usr/local/share/fonts/"})
  {
    for (const char *name : {"dejavu/DejaVuSansMono.ttf", "DejaVuSansMono.ttf", "noto/NotoSansMono-Regular.ttf", "NotoSansMono-Regular.ttf", "google-noto/NotoSansMono-Regular.ttf",
                             "liberation/LiberationMono-Regular.ttf", "LiberationMono-Regular.ttf", "liberation-mono/LiberationMono-Regular.ttf"})
      candidates.push_back(std::string(dir) + name);
  }
#endif
  for (const auto &path : candidates)
  {
    if (auto font = from_file(path))
      return font;
  }
  return std::unexpected(std::string("no fixed-width system font at any known path"));
}

namespace
{
// A font's measures in its own units: the extents, the advance of "0", and the decorations from the
// post and OS/2 tables, with stand-ins where a font leaves them out (G76).
struct design_metrics_t
{
  float upem = 1;
  float ascent = 0;
  float descent = 0;
  float line_gap = 0;
  float advance = 0;
  float underline_offset = 0;
  float underline_thickness = 0;
  float strikeout_offset = 0;
  float strikeout_thickness = 0;
};

design_metrics_t design_metrics(hb_face_t *face)
{
  design_metrics_t m;
  auto *hb = hb_font_create(face);
  m.upem = float(std::max(hb_face_get_upem(face), 1u));
  hb_font_extents_t extents{};
  hb_font_get_h_extents(hb, &extents);
  m.ascent = float(extents.ascender);
  m.descent = float(-extents.descender);
  m.line_gap = float(extents.line_gap);
  hb_codepoint_t zero = 0;
  if (hb_font_get_nominal_glyph(hb, '0', &zero))
    m.advance = float(hb_font_get_glyph_h_advance(hb, zero));
  hb_position_t value = 0;
  // HarfBuzz gives the post table's position as is: the middle of the line, as FreeType reads it,
  // negative below the baseline.
  m.underline_thickness = hb_ot_metrics_get_position(hb, HB_OT_METRICS_TAG_UNDERLINE_SIZE, &value) && value > 0 ? float(value) : m.upem / 14;
  m.underline_offset = hb_ot_metrics_get_position(hb, HB_OT_METRICS_TAG_UNDERLINE_OFFSET, &value) && value != 0 ? float(-value) : m.descent * 0.4f;
  m.strikeout_thickness = hb_ot_metrics_get_position(hb, HB_OT_METRICS_TAG_STRIKEOUT_SIZE, &value) && value > 0 ? float(value) : m.underline_thickness;
  // OS/2's position is the top of the stroke.
  m.strikeout_offset = hb_ot_metrics_get_position(hb, HB_OT_METRICS_TAG_STRIKEOUT_OFFSET, &value) && value > 0 ? float(value) - m.strikeout_thickness * 0.5f : m.ascent * 0.3f;
  hb_font_destroy(hb);
  return m;
}
} // namespace

font_t::metrics_t font_t::metrics(float size) const
{
  if (!_data || size <= 0)
    return {};
  const auto d = design_metrics(_data->face);
  const float per_unit = size / d.upem;
  metrics_t m;
  m.advance = d.advance * per_unit;
  m.ascent = d.ascent * per_unit;
  m.descent = d.descent * per_unit;
  m.line_gap = d.line_gap * per_unit;
  m.underline_offset = d.underline_offset * per_unit;
  m.underline_thickness = d.underline_thickness * per_unit;
  m.strikeout_offset = d.strikeout_offset * per_unit;
  m.strikeout_thickness = d.strikeout_thickness * per_unit;
  return m;
}

font_t::cell_metrics_t font_t::cell_metrics(float size, float scale) const
{
  if (!_data || size <= 0 || scale <= 0)
    return {};
  const auto d = design_metrics(_data->face);
  // In pixels as text is laid out (add_text): the size in 26.6 pixels, the ascent and descent each
  // rounded up to whole pixels.
  const float px = float(std::lround(size * scale * 64.0f)) / 64.0f;
  const float per_unit = px / d.upem;
  const int width = std::max(1, int(std::lround(d.advance * per_unit)));
  const int ascent = std::max(0, int(std::ceil(d.ascent * per_unit - 1e-3f)));
  const int descent = std::max(0, int(std::ceil(d.descent * per_unit - 1e-3f)));
  const int height = std::max(1, ascent + descent);
  const int underline_thickness = std::max(1, int(std::lround(d.underline_thickness * per_unit)));
  const int strikeout_thickness = std::max(1, int(std::lround(d.strikeout_thickness * per_unit)));
  // Tops from the cell's top, kept inside the cell.
  const int underline = std::clamp(ascent + int(std::lround(d.underline_offset * per_unit - underline_thickness * 0.5f)), 0, height - underline_thickness);
  const int strikeout = std::clamp(ascent - int(std::lround(d.strikeout_offset * per_unit + strikeout_thickness * 0.5f)), 0, height - strikeout_thickness);
  cell_metrics_t m;
  m.width = float(width) / scale;
  m.height = float(height) / scale;
  m.baseline = float(ascent) / scale;
  m.underline = float(underline) / scale;
  m.underline_thickness = float(underline_thickness) / scale;
  m.strikeout = float(strikeout) / scale;
  m.strikeout_thickness = float(strikeout_thickness) / scale;
  return m;
}

// ---- paint_t

void paint_t::text(std::uint16_t font, float x, float baseline, std::string_view text, float advance, color_t color)
{
  std::size_t i = 0;
  for (int column = 0; i < text.size(); ++column)
  {
    const char32_t cp = detail::next_code_point(text, i);
    if (cp != U' ')
      glyph(font, x + float(column) * advance, baseline, cp, color);
  }
}

// ---- path_t

path_t &path_t::move_to(float x, float y)
{
  close();
  _points.push_back({x, y, tag_t::on});
  return *this;
}

path_t &path_t::line_to(float x, float y)
{
  _points.push_back({x, y, tag_t::on});
  return *this;
}

path_t &path_t::quad_to(float cx, float cy, float x, float y)
{
  _points.push_back({cx, cy, tag_t::quad_control});
  _points.push_back({x, y, tag_t::on});
  return *this;
}

path_t &path_t::cubic_to(float c1x, float c1y, float c2x, float c2y, float x, float y)
{
  _points.push_back({c1x, c1y, tag_t::cubic_control});
  _points.push_back({c2x, c2y, tag_t::cubic_control});
  _points.push_back({x, y, tag_t::on});
  return *this;
}

path_t &path_t::close()
{
  const std::size_t start = _ends.empty() ? 0 : _ends.back();
  if (_points.size() > start)
    _ends.push_back(_points.size());
  return *this;
}

namespace detail
{
namespace
{
// The atlas grows from 512 square by doubling to this, and past it is emptied (G39).
constexpr int atlas_initial = 512;
constexpr int atlas_largest = 4096;
// Four horizontal subpixel positions per glyph (G27).
constexpr int subpixel_bins = 4;

struct glyph_key_t
{
  const font_data_t *font = nullptr;
  std::int32_t px64 = 0;
  std::uint32_t glyph = 0;
  std::uint32_t bin = 0;
  friend bool operator==(const glyph_key_t &, const glyph_key_t &) = default;
};

struct icon_key_t
{
  const path_t *path = nullptr;
  std::int32_t px64 = 0;
  friend bool operator==(const icon_key_t &, const icon_key_t &) = default;
};

struct key_hash_t
{
  static std::size_t mix(std::size_t seed, std::size_t value) noexcept
  {
    return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
  }
  std::size_t operator()(const glyph_key_t &k) const noexcept
  {
    std::size_t h = std::hash<const void *>()(k.font);
    h = mix(h, std::size_t(k.px64));
    h = mix(h, k.glyph);
    return mix(h, k.bin);
  }
  std::size_t operator()(const icon_key_t &k) const noexcept
  {
    return mix(std::hash<const void *>()(k.path), std::size_t(k.px64));
  }
};

// Where a rasterized glyph sits in the atlas, and where its top left is from the pen on the
// baseline, in pixels with y down.
struct cached_t
{
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  int left = 0;
  int top = 0;
};

struct sized_key_t
{
  const font_data_t *font = nullptr;
  std::int32_t px64 = 0;
  friend bool operator==(const sized_key_t &, const sized_key_t &) = default;
};

struct sized_hash_t
{
  std::size_t operator()(const sized_key_t &k) const noexcept
  {
    return key_hash_t::mix(std::hash<const void *>()(k.font), std::size_t(k.px64));
  }
};

struct code_point_key_t
{
  const font_data_t *font = nullptr;
  char32_t codepoint = 0;
  friend bool operator==(const code_point_key_t &, const code_point_key_t &) = default;
};

struct code_point_hash_t
{
  std::size_t operator()(const code_point_key_t &k) const noexcept
  {
    return key_hash_t::mix(std::hash<const void *>()(k.font), std::size_t(k.codepoint));
  }
};

int ceil_div(int value, int by) noexcept
{
  return (value + by - 1) / by;
}

// Rounded towards minus infinity, for pixel positions left of a canvas's anchor.
int floor_div(int value, int by) noexcept
{
  return value >= 0 ? value / by : -((-value + by - 1) / by);
}

// The fonts searched for a code point the font in use lacks (G77), most likely first: symbols,
// then the scripts a monospace font most often lacks. Paths, as the default fonts are found (G40).
std::vector<std::string> fallback_candidates()
{
  std::vector<std::string> out;
#if defined(_WIN32)
  const char *windir = std::getenv("WINDIR");
  const std::string fonts = std::string(windir ? windir : "C:\\Windows") + "\\Fonts\\";
  for (const char *name : {"seguisym.ttf", "seguiemj.ttf", "segoeui.ttf", "msyh.ttc", "YuGothR.ttc", "msgothic.ttc", "malgun.ttf", "simsun.ttc", "Nirmala.ttc", "Nirmala.ttf", "seguihis.ttf", "ebrima.ttf", "cambria.ttc", "arial.ttf"})
    out.push_back(fonts + name);
#elif defined(__APPLE__)
  out = {"/System/Library/Fonts/Apple Symbols.ttf", "/System/Library/Fonts/Supplemental/Arial Unicode.ttf", "/Library/Fonts/Arial Unicode.ttf", "/System/Library/Fonts/PingFang.ttc",
         "/System/Library/Fonts/Hiragino Sans GB.ttc", "/System/Library/Fonts/AppleSDGothicNeo.ttc", "/System/Library/Fonts/STHeiti Light.ttc", "/System/Library/Fonts/Supplemental/STIXGeneral.otf",
         "/System/Library/Fonts/Geneva.ttf"};
#else
  const char *dirs[] = {"/usr/share/fonts/truetype/dejavu/", "/usr/share/fonts/TTF/", "/usr/share/fonts/truetype/noto/", "/usr/share/fonts/noto/", "/usr/share/fonts/google-noto/", "/usr/share/fonts/opentype/noto/",
                        "/usr/share/fonts/noto-cjk/", "/usr/share/fonts/google-noto-cjk/", "/usr/share/fonts/truetype/", "/usr/share/fonts/OTF/", "/usr/share/fonts/", "/usr/local/share/fonts/"};
  for (const char *name : {"DejaVuSans.ttf", "NotoSans-Regular.ttf", "NotoSansSymbols-Regular.ttf", "NotoSansSymbols2-Regular.ttf", "NotoSansMath-Regular.ttf", "NotoSansCJK-Regular.ttc", "NotoSansCJKsc-Regular.otf",
                           "NotoSansCJKjp-Regular.otf", "DroidSansFallbackFull.ttf", "Symbola.ttf", "FreeSerif.ttf", "unifont.otf"})
  {
    for (const char *dir : dirs)
      out.push_back(std::string(dir) + name);
  }
#endif
  return out;
}

// The fallback fonts that exist, each loaded the first time it is asked for and kept for the life
// of the process, since a font_t is safe to share and the files are large.
struct fallbacks_t
{
  std::mutex mutex;
  bool listed = false;
  std::vector<std::string> paths;
  std::deque<std::optional<font_t>> fonts;
  std::vector<bool> tried;
};

fallbacks_t &fallbacks()
{
  static fallbacks_t instance;
  return instance;
}
} // namespace

namespace
{
// Which candidates exist, found once. Called with the mutex held.
void list_fallbacks(fallbacks_t &all)
{
  if (!all.listed)
  {
    all.listed = true;
    std::error_code ec;
    for (auto &path : fallback_candidates())
    {
      if (std::filesystem::is_regular_file(to_path(path), ec) && std::find(all.paths.begin(), all.paths.end(), path) == all.paths.end())
        all.paths.push_back(std::move(path));
    }
    all.fonts.resize(all.paths.size());
    all.tried.resize(all.paths.size());
  }
}
} // namespace

const font_t *fallback_font(std::size_t index)
{
  auto &all = fallbacks();
  std::lock_guard lock(all.mutex);
  list_fallbacks(all);
  if (index >= all.paths.size())
    return nullptr;
  if (!all.tried[index])
  {
    all.tried[index] = true;
    if (auto font = font_t::from_file(all.paths[index]))
      all.fonts[index] = std::move(*font);
  }
  // A file that does not load is passed over as if it were not there.
  return all.fonts[index] ? &*all.fonts[index] : nullptr;
}

namespace
{
std::size_t fallback_count()
{
  auto &all = fallbacks();
  std::lock_guard lock(all.mutex);
  list_fallbacks(all);
  return all.paths.size();
}

void move_prim(prim_t &prim, float dx, float dy) noexcept
{
  prim.x += dx;
  prim.y += dy;
  if (prim.kind == prim_kind_t::segment)
  {
    prim.radii[0] += dx;
    prim.radii[1] += dy;
  }
}
} // namespace

// The image atlas (G55): RGBA, four bytes a texel, packed in shelves like the glyph atlas. Each image
// gets a border of its own edge texels, so bilinear filtering at its edge never reads a neighbour.
// Starts small, doubles as it fills, and empties when it cannot grow, which the caller turns into a
// second layout as it does for glyphs.
struct image_atlas_t
{
  static constexpr int initial = 512;
  static constexpr int largest = 4096;

  struct shelf_t
  {
    int y = 0;
    int height = 0;
    int x = 0;
  };
  struct region_t
  {
    std::shared_ptr<const image_data_t> data;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    std::uint64_t version = 0;
  };

  std::vector<std::uint8_t> pixels;
  int width = 0;
  int height = 0;
  std::vector<shelf_t> shelves;
  int next_y = 0;
  rect_t dirty;
  std::uint64_t generation = 0;
  std::unordered_map<const image_data_t *, region_t> regions;

  void empty()
  {
    std::fill(pixels.begin(), pixels.end(), std::uint8_t(0));
    shelves.clear();
    next_y = 0;
    regions.clear();
    dirty = {};
    ++generation;
  }

  void grow(int side)
  {
    std::vector<std::uint8_t> grown(std::size_t(side) * side * 4, 0);
    for (int row = 0; row < height; ++row)
      std::memcpy(&grown[std::size_t(row) * side * 4], &pixels[std::size_t(row) * width * 4], std::size_t(width) * 4);
    pixels = std::move(grown);
    width = height = side;
    dirty = {};
    ++generation;
  }

  bool place(int w, int h, int &x, int &y)
  {
    for (auto &shelf : shelves)
    {
      if (h <= shelf.height && h * 4 >= shelf.height * 3 && shelf.x + w <= width)
      {
        x = shelf.x;
        y = shelf.y;
        shelf.x += w;
        return true;
      }
    }
    if (next_y + h <= height && w <= width)
    {
      shelves.push_back({next_y, h, w});
      x = 0;
      y = next_y;
      next_y += h;
      return true;
    }
    return false;
  }

  void touched(const rect_t &added)
  {
    if (dirty.width == 0 || dirty.height == 0)
    {
      dirty = added;
      return;
    }
    const int x0 = std::min(dirty.x, added.x);
    const int y0 = std::min(dirty.y, added.y);
    const int x1 = std::max(dirty.x + dirty.width, added.x + added.width);
    const int y1 = std::max(dirty.y + dirty.height, added.y + added.height);
    dirty = {x0, y0, x1 - x0, y1 - y0};
  }

  // Where `data` is, its texels written there if they are new. Null when it does not fit; `emptied`
  // says the atlas had to be emptied to make room, which invalidates what was laid out before.
  const region_t *find(const std::shared_ptr<const image_data_t> &data, bool &emptied)
  {
    const auto *d = data.get();
    if (!d || d->width <= 0 || d->height <= 0 || d->rgba.size() < std::size_t(d->width) * std::size_t(d->height) * 4)
      return nullptr;
    auto it = regions.find(d);
    if (it != regions.end() && it->second.width == d->width && it->second.height == d->height && it->second.version == d->version)
      return &it->second;
    const int w = d->width + 2;
    const int h = d->height + 2;
    if (w > largest || h > largest)
      return nullptr;
    int x = 0, y = 0;
    if (it != regions.end() && it->second.width == d->width && it->second.height == d->height)
    {
      // Same size, new pixels: written where it already is.
      x = it->second.x - 1;
      y = it->second.y - 1;
    }
    else
    {
      if (width == 0)
        grow(initial);
      while (!place(w, h, x, y))
      {
        if (width >= largest)
        {
          if (emptied)
            return nullptr;
          empty();
          emptied = true;
          continue;
        }
        grow(width * 2);
      }
    }
    // The texels, then the border: each edge row and column copied one texel outward.
    const auto *src = d->rgba.data();
    for (int row = -1; row <= d->height; ++row)
    {
      const int from_row = std::clamp(row, 0, d->height - 1);
      auto *line = &pixels[(std::size_t(y + 1 + row) * width + std::size_t(x)) * 4];
      const auto *source = src + std::size_t(from_row) * d->width * 4;
      std::memcpy(line, source, 4);
      std::memcpy(line + 4, source, std::size_t(d->width) * 4);
      std::memcpy(line + std::size_t(d->width + 1) * 4, source + std::size_t(d->width - 1) * 4, 4);
    }
    touched({x, y, w, h});
    auto &region = regions[d];
    region = {data, x + 1, y + 1, d->width, d->height, d->version};
    return &region;
  }
};

struct text_engine_t::impl_t
{
  FT_Library library = nullptr;
  hb_buffer_t *buffer = nullptr;
  float scale = 1;

  struct face_t
  {
    font_t font; // keeps the bytes FreeType reads alive
    FT_Face face = nullptr;
    std::int32_t px64 = 0;
  };
  std::unordered_map<const font_data_t *, face_t> faces;
  std::unordered_map<sized_key_t, hb_font_t *, sized_hash_t> sized;
  // HarfBuzz fonts at no particular size, for looking up a code point's glyph.
  std::unordered_map<const font_data_t *, hb_font_t *> cmaps;
  // What each code point of each font is drawn with (G77), and every font named there.
  std::unordered_map<code_point_key_t, resolved_t, code_point_hash_t> resolved;
  std::unordered_map<const font_data_t *, font_t> kept;

  // What each paint became (G74), by its address, which the shared pointer keeps from being reused.
  // `epoch` is the atlas's: prims made before it was emptied or the scale changed name nothing.
  struct canvas_entry_t
  {
    std::shared_ptr<const paint_t> paint;
    std::vector<prim_t> prims;
    std::uint64_t epoch = 0;
    std::uint64_t used = 0;
  };
  std::unordered_map<const paint_t *, canvas_entry_t> canvases;
  std::uint64_t epoch = 0;
  std::uint64_t layouts = 0;

  // The atlas: shelves of rows, each as tall as the first glyph put on it.
  struct shelf_t
  {
    int y = 0;
    int height = 0;
    int x = 0;
  };
  std::vector<std::uint8_t> pixels;
  int width = 0;
  int height = 0;
  std::vector<shelf_t> shelves;
  int next_y = 0;
  rect_t dirty;
  std::uint64_t generation = 0;
  bool reset = false;
  image_atlas_t images;

  std::unordered_map<glyph_key_t, cached_t, key_hash_t> glyphs;
  std::unordered_map<icon_key_t, cached_t, key_hash_t> icons;
  std::vector<std::shared_ptr<const path_t>> icon_paths; // keep the keys' addresses from being reused

  // Heatmaps, by their data's address, held so the address is not reused while the region is.
  struct heatmap_region_t
  {
    std::shared_ptr<const heatmap_data_t> data;
    int x = 0;
    int y = 0;
    int columns = 0;
    int rows = 0;
    std::uint64_t version = 0;
    float low = 0;
    float high = 0;
  };
  std::unordered_map<const heatmap_data_t *, heatmap_region_t> heatmaps;

  impl_t()
  {
    FT_Init_FreeType(&library);
    buffer = hb_buffer_create();
  }

  ~impl_t()
  {
    for (auto &[key, font] : sized)
      hb_font_destroy(font);
    for (auto &[key, font] : cmaps)
      hb_font_destroy(font);
    for (auto &[key, face] : faces)
      FT_Done_Face(face.face);
    hb_buffer_destroy(buffer);
    if (library)
      FT_Done_FreeType(library);
  }

  face_t *face_for(const font_t &font)
  {
    auto it = faces.find(font.data());
    if (it != faces.end())
      return &it->second;
    if (!library)
      return nullptr;
    face_t entry;
    entry.font = font;
    const auto &bytes = *font.data()->bytes;
    if (FT_New_Memory_Face(library, reinterpret_cast<const FT_Byte *>(bytes.data()), FT_Long(bytes.size()), FT_Long(font.data()->index), &entry.face) != 0)
      return nullptr;
    return &faces.emplace(font.data(), std::move(entry)).first->second;
  }

  std::uint32_t nominal(const font_t &font, char32_t codepoint)
  {
    auto it = cmaps.find(font.data());
    if (it == cmaps.end())
      it = cmaps.emplace(font.data(), hb_font_create(font.data()->face)).first;
    hb_codepoint_t glyph = 0;
    if (!hb_font_get_nominal_glyph(it->second, codepoint, &glyph))
      return 0;
    return glyph;
  }

  // A copy of a font this engine points at, kept for as long as the engine is, so a pointer to it
  // stays valid and the address it is keyed by is never reused.
  const font_t *keep(const font_t &font)
  {
    return &kept.try_emplace(font.data(), font).first->second;
  }

  resolved_t resolve(const font_t &font, char32_t codepoint)
  {
    const code_point_key_t key{font.data(), codepoint};
    if (auto it = resolved.find(key); it != resolved.end())
      return it->second;
    resolved_t answer{keep(font), nominal(font, codepoint)};
    if (answer.glyph == 0)
    {
      // The first fallback font that has it, in the style asked for, so a bold cell stays bold.
      const std::size_t count = fallback_count();
      for (std::size_t i = 0; i < count && answer.glyph == 0; ++i)
      {
        const font_t *candidate = fallback_font(i);
        if (!candidate || nominal(*candidate, codepoint) == 0)
          continue;
        const font_t *use = font.style() == candidate->style() ? candidate : keep(candidate->styled(font.style()));
        answer = {use, nominal(*use, codepoint)};
      }
      // Nothing has it: the font's own .notdef, which is what a reader expects to see.
      if (answer.glyph == 0)
        answer = {keep(font), 0};
    }
    resolved.emplace(key, answer);
    return answer;
  }

  hb_font_t *sized_font(const font_t &font, std::int32_t px64)
  {
    const sized_key_t key{font.data(), px64};
    auto it = sized.find(key);
    if (it != sized.end())
      return it->second;
    // HarfBuzz's own OpenType font functions, scaled so positions come back in 26.6 pixels.
    auto *hb = hb_font_create(font.data()->face);
    hb_font_set_scale(hb, px64, px64);
    sized.emplace(key, hb);
    return hb;
  }

  // ---- the atlas
  void empty_atlas()
  {
    std::fill(pixels.begin(), pixels.end(), std::uint8_t(0));
    shelves.clear();
    next_y = 0;
    glyphs.clear();
    icons.clear();
    icon_paths.clear();
    heatmaps.clear();
    canvases.clear();
    ++epoch;
    dirty = {};
    ++generation;
  }

  void grow(int side)
  {
    std::vector<std::uint8_t> grown(std::size_t(side) * side, 0);
    for (int row = 0; row < height; ++row)
      std::memcpy(&grown[std::size_t(row) * side], &pixels[std::size_t(row) * width], std::size_t(width));
    pixels = std::move(grown);
    width = height = side;
    dirty = {};
    ++generation;
  }

  bool place(int w, int h, int &x, int &y)
  {
    // One texel of space around each rectangle; the shader reads texels exactly, but a neighbour
    // touching it would show at any scale where rounding nudges a quad.
    const int pw = w + 1;
    const int ph = h + 1;
    for (auto &shelf : shelves)
    {
      if (ph <= shelf.height && ph * 4 >= shelf.height * 3 && shelf.x + pw <= width)
      {
        x = shelf.x;
        y = shelf.y;
        shelf.x += pw;
        return true;
      }
    }
    if (next_y + ph <= height && pw <= width)
    {
      shelves.push_back({next_y, ph, pw});
      x = 0;
      y = next_y;
      next_y += ph;
      return true;
    }
    return false;
  }

  bool allocate(int w, int h, int &x, int &y)
  {
    if (width == 0)
      grow(atlas_initial);
    while (!place(w, h, x, y))
    {
      if (width >= atlas_largest)
      {
        if (reset || w + 1 > width || h + 1 > height)
          return false; // a second fill in one layout, or a glyph larger than the atlas: drop it
        empty_atlas();
        reset = true;
        continue;
      }
      grow(width * 2);
    }
    return true;
  }

  void store(const std::uint8_t *source, int pitch, int w, int h, cached_t &into)
  {
    if (!allocate(w, h, into.x, into.y))
    {
      into.width = into.height = 0;
      return;
    }
    for (int row = 0; row < h; ++row)
      std::memcpy(&pixels[std::size_t(into.y + row) * width + into.x], source + std::ptrdiff_t(row) * pitch, std::size_t(w));
    into.width = w;
    into.height = h;
    touched({into.x, into.y, w, h});
  }

  // Grow the rectangle the renderer uploads next to cover `added`.
  void touched(const rect_t &added)
  {
    if (dirty.width == 0 || dirty.height == 0)
    {
      dirty = added;
      return;
    }
    const int x0 = std::min(dirty.x, added.x);
    const int y0 = std::min(dirty.y, added.y);
    const int x1 = std::max(dirty.x + dirty.width, added.x + added.width);
    const int y1 = std::max(dirty.y + dirty.height, added.y + added.height);
    dirty = {x0, y0, x1 - x0, y1 - y0};
  }

  // ---- rasterizing
  const cached_t &glyph(const font_t &font, std::int32_t px64, std::uint32_t id, std::uint32_t bin)
  {
    const glyph_key_t key{font.data(), px64, id, bin};
    if (auto it = glyphs.find(key); it != glyphs.end())
      return it->second;
    cached_t cached;
    if (auto *face = face_for(font))
    {
      if (face->px64 != px64)
      {
        FT_Set_Char_Size(face->face, 0, px64, 72, 72);
        face->px64 = px64;
      }
      // Light hinting moves outlines vertically only, so HarfBuzz's unhinted advances still fit
      // them; the subpixel offset is applied to the outline before it is rendered.
      if (FT_Load_Glyph(face->face, id, FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP) == 0 && face->face->glyph->format == FT_GLYPH_FORMAT_OUTLINE)
      {
        auto *slot = face->face->glyph;
        // A synthetic face (G75): slanted by FreeType's own oblique shear, about 12 degrees, then
        // widened by a twentieth of the size and at least two thirds of a pixel, sideways only, so
        // the glyph keeps its height and a bold cell does not reach into the row below.
        if (font.data()->oblique)
        {
          FT_Matrix shear{0x10000, 0x0366A, 0, 0x10000};
          FT_Outline_Transform(&slot->outline, &shear);
        }
        if (font.data()->embolden)
          FT_Outline_EmboldenXY(&slot->outline, std::max<FT_Pos>(px64 / 20, 43), 0);
        FT_Outline_Translate(&slot->outline, FT_Pos(bin * 64 / subpixel_bins), 0);
        if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) == 0 && slot->bitmap.width > 0 && slot->bitmap.rows > 0)
        {
          const auto &bitmap = slot->bitmap;
          store(bitmap.buffer, bitmap.pitch, int(bitmap.width), int(bitmap.rows), cached);
          cached.left = slot->bitmap_left;
          cached.top = -slot->bitmap_top;
        }
      }
    }
    return glyphs.emplace(key, cached).first->second;
  }

  const cached_t &icon(const std::shared_ptr<const path_t> &path, std::int32_t px64, int side)
  {
    const icon_key_t key{path.get(), px64};
    if (auto it = icons.find(key); it != icons.end())
      return it->second;
    cached_t cached;
    const auto points = path->points();
    std::vector<std::size_t> ends(path->ends().begin(), path->ends().end());
    if (ends.empty() ? !points.empty() : ends.back() < points.size())
      ends.push_back(points.size());
    FT_Outline outline{};
    if (library && !points.empty() && side > 0 && FT_Outline_New(library, FT_UInt(points.size()), FT_Int(ends.size()), &outline) == 0)
    {
      const float px = float(px64) / 64.0f;
      for (std::size_t i = 0; i < points.size(); ++i)
      {
        // Unit square, y down, to 26.6 pixels with y up and the bitmap's bottom at zero.
        outline.points[i].x = FT_Pos(std::lround(points[i].x * px * 64.0f));
        outline.points[i].y = FT_Pos(std::lround((float(side) - points[i].y * px) * 64.0f));
        const auto tag = points[i].tag;
        using tag_type = std::remove_reference_t<decltype(*outline.tags)>;
        outline.tags[i] = tag_type(tag == path_t::tag_t::on ? FT_CURVE_TAG_ON : tag == path_t::tag_t::quad_control ? FT_CURVE_TAG_CONIC : FT_CURVE_TAG_CUBIC);
      }
      using contour_type = std::remove_reference_t<decltype(*outline.contours)>;
      for (std::size_t i = 0; i < ends.size(); ++i)
        outline.contours[i] = contour_type(ends[i] - 1);
      if (path->even_odd)
        outline.flags |= FT_OUTLINE_EVEN_ODD_FILL;
      std::vector<std::uint8_t> raster(std::size_t(side) * side, 0);
      FT_Bitmap bitmap{};
      bitmap.rows = unsigned(side);
      bitmap.width = unsigned(side);
      bitmap.pitch = side;
      bitmap.buffer = raster.data();
      bitmap.num_grays = 256;
      bitmap.pixel_mode = FT_PIXEL_MODE_GRAY;
      if (FT_Outline_Get_Bitmap(library, &outline, &bitmap) == 0)
        store(raster.data(), side, side, side, cached);
      FT_Outline_Done(library, &outline);
    }
    icon_paths.push_back(path);
    return icons.emplace(key, cached).first->second;
  }
};

text_engine_t::text_engine_t()
  : _impl(std::make_unique<impl_t>())
{
}

text_engine_t::~text_engine_t() = default;

void text_engine_t::set_scale(float scale)
{
  if (scale == _impl->scale)
    return;
  _impl->scale = scale;
  _impl->canvases.clear();
  ++_impl->epoch;
  if (_impl->width > 0)
    _impl->empty_atlas();
}

text_engine_t::laid_out_t text_engine_t::add_text(const text_t &text, const font_t &fallback, float x, float y, std::vector<prim_t> &out)
{
  auto &impl = *_impl;
  const font_t &given = text.font ? text.font : fallback;
  if (!given || text.size <= 0)
    return {};
  const font_t font = text.style == font_style_t::regular ? given : given.styled(text.style);
  const float scale = impl.scale;
  const auto px64 = std::int32_t(std::lround(text.size * scale * 64.0f));
  auto *hb = impl.sized_font(font, px64);
  hb_font_extents_t extents{};
  hb_font_get_h_extents(hb, &extents);
  // The baseline sits on a whole pixel, the ascender above it rounded up.
  const int baseline = ceil_div(std::max(extents.ascender, 0), 64);
  const int below = ceil_div(std::max(-extents.descender, 0), 64);

  hb_buffer_reset(impl.buffer);
  hb_buffer_add_utf8(impl.buffer, text.text.data(), int(text.text.size()), 0, int(text.text.size()));
  hb_buffer_guess_segment_properties(impl.buffer);
  // On a grid each code point is a cell of its own, so nothing may join two of them.
  const bool grid = text.advance > 0;
  static const hb_feature_t no_ligatures[] = {{HB_TAG('l', 'i', 'g', 'a'), 0, 0, ~0u}, {HB_TAG('c', 'l', 'i', 'g'), 0, 0, ~0u}, {HB_TAG('c', 'a', 'l', 't'), 0, 0, ~0u}};
  hb_shape(hb, impl.buffer, grid ? no_ligatures : nullptr, grid ? 3 : 0);
  unsigned count = 0;
  const auto *infos = hb_buffer_get_glyph_infos(impl.buffer, &count);
  const auto *positions = hb_buffer_get_glyph_positions(impl.buffer, &count);

  // The cell each byte's code point starts, for a grid: HarfBuzz reports clusters as byte offsets.
  std::vector<std::uint32_t> cells;
  std::uint32_t columns = 0;
  if (grid)
  {
    cells.resize(text.text.size() + 1);
    for (std::size_t b = 0; b < text.text.size(); ++b)
    {
      if ((static_cast<unsigned char>(text.text[b]) & 0xc0) != 0x80 && b > 0)
        ++columns;
      cells[b] = columns;
    }
    columns = text.text.empty() ? 0 : columns + 1;
  }
  const double cell64 = double(text.advance) * scale * 64.0;

  // Glyphs come back in visual order, left to right, whatever the direction.
  const std::size_t first = out.size();
  std::int64_t pen = 0;
  for (unsigned i = 0; i < count; ++i)
  {
    if (grid)
      pen = std::int64_t(std::llround(cells[std::min<std::size_t>(infos[i].cluster, text.text.size())] * cell64));
    const std::int64_t at = pen + positions[i].x_offset;
    const std::int64_t whole = at >= 0 ? at / 64 : -((-at + 63) / 64);
    const auto bin = std::uint32_t(((at - whole * 64) * subpixel_bins) / 64);
    const font_t *drawn = &font;
    std::uint32_t id = infos[i].codepoint;
    if (grid && id == 0 && infos[i].cluster < text.text.size())
    {
      // A cell is a code point of its own, so one the font lacks can come from another font (G77).
      std::size_t at_byte = infos[i].cluster;
      const auto resolved = impl.resolve(font, next_code_point(text.text, at_byte));
      drawn = resolved.font;
      id = resolved.glyph;
    }
    const auto &cached = impl.glyph(*drawn, px64, id, bin);
    pen += positions[i].x_advance;
    if (cached.width == 0)
      continue;
    prim_t prim;
    prim.kind = prim_kind_t::coverage;
    prim.x = x;
    prim.y = y;
    prim.width = float(cached.width);
    prim.height = float(cached.height);
    prim.radii[0] = float(cached.x);
    prim.radii[1] = float(cached.y);
    prim.radii[2] = float(whole + cached.left);
    prim.radii[3] = float(baseline + cached.top - int(std::lround(positions[i].y_offset / 64.0)));
    prim.fill = text.color;
    out.push_back(prim);
  }
  if (text.tight && out.size() > first)
  {
    // The ink's bounds in pixels from the anchor, and every glyph moved by whole pixels so they
    // start at the anchor: still one texel per pixel.
    float left = out[first].radii[2];
    float top = out[first].radii[3];
    float right = left;
    float bottom = top;
    for (std::size_t i = first; i < out.size(); ++i)
    {
      left = std::min(left, out[i].radii[2]);
      top = std::min(top, out[i].radii[3]);
      right = std::max(right, out[i].radii[2] + out[i].width);
      bottom = std::max(bottom, out[i].radii[3] + out[i].height);
    }
    for (std::size_t i = first; i < out.size(); ++i)
    {
      out[i].radii[2] -= left;
      out[i].radii[3] -= top;
    }
    return {{int(std::ceil((right - left) / scale - 1e-3f)), int(std::ceil((bottom - top) / scale - 1e-3f))}, (float(baseline) - top) / scale};
  }
  const int height_px = baseline + below;
  if (grid)
    return {{int(std::ceil(float(columns) * text.advance - 1e-3f)), int(std::ceil(height_px / scale - 1e-3f))}, float(baseline) / scale};
  const int width_px = int(ceil_div(int(std::max<std::int64_t>(pen, 0)), 64));
  return {{int(std::ceil(width_px / scale - 1e-3f)), int(std::ceil(height_px / scale - 1e-3f))}, float(baseline) / scale};
}

extent_t text_engine_t::add_icon(const icon_t &icon, float x, float y, std::vector<prim_t> &out)
{
  auto &impl = *_impl;
  const int logical = int(std::ceil(icon.size - 1e-3f));
  if (!icon.path || icon.size <= 0)
    return {logical, logical};
  const float px = icon.size * impl.scale;
  const auto px64 = std::int32_t(std::lround(px * 64.0f));
  const int side = int(std::ceil(px - 1e-3f));
  const auto &cached = impl.icon(icon.path, px64, side);
  if (cached.width > 0)
  {
    prim_t prim;
    prim.kind = prim_kind_t::coverage;
    prim.x = x;
    prim.y = y;
    prim.width = float(cached.width);
    prim.height = float(cached.height);
    prim.radii[0] = float(cached.x);
    prim.radii[1] = float(cached.y);
    prim.fill = icon.color;
    out.push_back(prim);
  }
  return {logical, logical};
}

void text_engine_t::add_heatmap(const heatmap_t &map, float x, float y, std::vector<prim_t> &out)
{
  auto &impl = *_impl;
  const auto *data = map.data.get();
  if (!data || data->columns <= 0 || data->rows <= 0 || data->values.size() < std::size_t(data->columns) * std::size_t(data->rows))
    return;
  auto it = impl.heatmaps.find(data);
  if (it == impl.heatmaps.end() || it->second.columns != data->columns || it->second.rows != data->rows)
  {
    impl_t::heatmap_region_t region;
    if (!impl.allocate(data->columns, data->rows, region.x, region.y))
      return;
    region.data = map.data;
    region.columns = data->columns;
    region.rows = data->rows;
    // Never matches a real version, so the values are written below.
    region.version = ~data->version;
    // Allocating may have emptied the atlas, and the map with it.
    it = impl.heatmaps.insert_or_assign(data, std::move(region)).first;
  }
  auto &region = it->second;
  if (region.version != data->version || region.low != map.low || region.high != map.high)
  {
    const float span = map.high - map.low;
    const float to_level = span != 0 ? 255.0f / span : 0.0f;
    for (int row = 0; row < data->rows; ++row)
    {
      auto *line = &impl.pixels[std::size_t(region.y + row) * impl.width + region.x];
      const float *values = &data->values[std::size_t(row) * data->columns];
      for (int column = 0; column < data->columns; ++column)
        line[column] = std::uint8_t(std::clamp((values[column] - map.low) * to_level + 0.5f, 0.0f, 255.0f));
    }
    region.version = data->version;
    region.low = map.low;
    region.high = map.high;
    impl.touched({region.x, region.y, data->columns, data->rows});
  }
  prim_t prim;
  prim.kind = prim_kind_t::heatmap;
  prim.x = x;
  prim.y = y;
  prim.width = float(map.size.width);
  prim.height = float(map.size.height);
  prim.radii[0] = float(region.x);
  prim.radii[1] = float(region.y);
  prim.radii[2] = float(data->columns);
  prim.radii[3] = float(data->rows);
  prim.fill = {1, 1, 1, map.opacity};
  out.push_back(prim);
}

void text_engine_t::add_image(const image_t &image, float x, float y, std::vector<prim_t> &out)
{
  auto &impl = *_impl;
  bool emptied = false;
  const auto *region = impl.images.find(image.data, emptied);
  if (emptied)
    impl.reset = true;
  if (!region || image.size.width <= 0 || image.size.height <= 0)
    return;
  prim_t prim;
  prim.kind = prim_kind_t::image;
  prim.x = x;
  prim.y = y;
  prim.width = float(image.size.width);
  prim.height = float(image.size.height);
  prim.radii[0] = float(region->x);
  prim.radii[1] = float(region->y);
  prim.radii[2] = float(region->width);
  prim.radii[3] = float(region->height);
  prim.border_width = image.radius;
  prim.fill = {1, 1, 1, image.opacity};
  out.push_back(prim);
}

text_engine_t::resolved_t text_engine_t::resolve(const font_t &font, char32_t codepoint)
{
  if (!font)
    return {};
  return _impl->resolve(font, codepoint);
}

void text_engine_t::begin_layout()
{
  auto &impl = *_impl;
  ++impl.layouts;
  // A paint no layout has asked for in the last two is let go; the next that asks converts it.
  std::erase_if(impl.canvases, [&](const auto &entry) { return entry.second.used + 2 < impl.layouts; });
}

void text_engine_t::add_canvas(const canvas_t &canvas, float x, float y, std::vector<prim_t> &out)
{
  auto &impl = *_impl;
  const auto *paint = canvas.paint.get();
  if (!paint)
    return;
  const std::size_t first = out.size();
  if (auto it = impl.canvases.find(paint); it != impl.canvases.end() && it->second.epoch == impl.epoch)
  {
    it->second.used = impl.layouts;
    out.insert(out.end(), it->second.prims.begin(), it->second.prims.end());
  }
  else
  {
    const auto epoch = impl.epoch;
    std::vector<prim_t> prims;
    paint_prims(*paint, prims);
    // Converting may have filled and emptied the atlas, and then the layout runs again anyway.
    if (impl.epoch == epoch)
      impl.canvases.insert_or_assign(paint, impl_t::canvas_entry_t{canvas.paint, prims, epoch, impl.layouts});
    out.insert(out.end(), prims.begin(), prims.end());
  }
  for (std::size_t i = first; i < out.size(); ++i)
    move_prim(out[i], x, y);
}

// A paint as prims anchored at (0, 0) (G74, G76). Every rectangle and every line but a curly one is a
// pixels prim whose edges are whole pixels from the anchor, so neighbours share their boundaries
// exactly; a glyph is a coverage prim on a whole-pixel baseline at one of four subpixel pens.
void text_engine_t::paint_prims(const paint_t &paint, std::vector<prim_t> &out)
{
  auto &impl = *_impl;
  const float s = impl.scale;
  const auto fonts = paint.fonts();
  std::vector<std::int32_t> px64(fonts.size());
  for (std::size_t i = 0; i < fonts.size(); ++i)
    px64[i] = std::int32_t(std::lround(fonts[i].size * s * 64.0f));

  const auto block = [&](int left, int top, int right, int bottom, const color_t &color)
  {
    if (right <= left || bottom <= top || color.a <= 0)
      return;
    prim_t prim;
    prim.kind = prim_kind_t::pixels;
    prim.width = float(right - left);
    prim.height = float(bottom - top);
    prim.radii[2] = float(left);
    prim.radii[3] = float(top);
    prim.fill = color;
    out.push_back(prim);
  };
  // A span in units to whole pixels; one that has any length is at least a pixel.
  const auto snap = [s](float from, float length, int &low, int &high)
  {
    low = int(std::lround(from * s));
    high = int(std::lround((from + length) * s));
    if (length > 0 && high <= low)
      high = low + 1;
  };

  for (const auto &command : paint.commands())
  {
    switch (command.op)
    {
    case paint_t::op_t::rect:
    {
      int left = 0, right = 0, top = 0, bottom = 0;
      snap(command.x, command.width, left, right);
      snap(command.y, command.height, top, bottom);
      block(left, top, right, bottom, command.color);
      break;
    }
    case paint_t::op_t::glyph:
    {
      if (command.font >= fonts.size() || command.codepoint == U' ' || !fonts[command.font].font)
        break;
      const auto resolved = impl.resolve(fonts[command.font].font, command.codepoint);
      const std::int64_t pen = std::llround(double(command.x) * s * 64.0);
      const std::int64_t whole = pen >= 0 ? pen / 64 : -((-pen + 63) / 64);
      const auto bin = std::uint32_t(((pen - whole * 64) * subpixel_bins) / 64);
      const auto &cached = impl.glyph(*resolved.font, px64[command.font], resolved.glyph, bin);
      if (cached.width == 0)
        break;
      prim_t prim;
      prim.kind = prim_kind_t::coverage;
      prim.width = float(cached.width);
      prim.height = float(cached.height);
      prim.radii[0] = float(cached.x);
      prim.radii[1] = float(cached.y);
      prim.radii[2] = float(whole + cached.left);
      prim.radii[3] = float(std::lround(command.y * s) + cached.top);
      prim.fill = command.color;
      out.push_back(prim);
      break;
    }
    case paint_t::op_t::line:
    {
      int left = 0, right = 0;
      snap(command.x, command.width, left, right);
      const int t = std::max(1, int(std::lround(command.height * s)));
      const int top = int(std::lround(command.y * s));
      switch (command.style)
      {
      case line_style_t::solid:
        block(left, top, right, top + t, command.color);
        break;
      case line_style_t::double_line:
        block(left, top, right, top + t, command.color);
        block(left, top + 2 * t, right, top + 3 * t, command.color);
        break;
      case line_style_t::dotted:
      case line_style_t::dashed:
      {
        // On and off from the canvas's left edge, so lines drawn a cell at a time join up.
        const bool dots = command.style == line_style_t::dotted;
        const int on = dots ? t : std::max(4, 3 * t);
        const int period = dots ? 2 * t : on + std::max(2, 2 * t);
        for (int k = floor_div(left, period); k * period < right; ++k)
          block(std::max(left, k * period), top, std::min(right, k * period + on), top + t, command.color);
        break;
      }
      case line_style_t::curly:
      {
        // A wave between the line's first and third thickness, its stroke a thickness wide: a
        // zigzag of mitred segments whose peaks fall at whole multiples of half a period from the
        // canvas's left edge, so lines drawn a cell at a time continue one another.
        if (right <= left)
          break;
        const int half = 2 * t + 1;
        const float high = float(top) + float(t) * 0.5f;
        const float low = float(top) + float(t) * 2.5f;
        const auto wave = [&](float px)
        {
          const float k = std::floor(px / float(half));
          const float f = px / float(half) - k;
          const bool rising = std::fmod(std::fabs(k), 2.0f) == 1.0f;
          return rising ? low + (high - low) * f : high + (low - high) * f;
        };
        std::vector<vec2_t> points;
        points.push_back({float(left) / s, wave(float(left)) / s});
        for (int k = floor_div(left, half) + 1; k * half < right; ++k)
          points.push_back({float(k * half) / s, wave(float(k * half)) / s});
        points.push_back({float(right) / s, wave(float(right)) / s});
        append_line_segments(points, 0, 0, float(t) / s, command.color, out);
        break;
      }
      }
      break;
    }
    }
  }
}

bool text_engine_t::take_reset() noexcept
{
  return std::exchange(_impl->reset, false);
}

atlas_view_t text_engine_t::atlas() const noexcept
{
  const auto &impl = *_impl;
  return {impl.pixels, {impl.width, impl.height}, impl.dirty, impl.generation};
}

atlas_view_t text_engine_t::images() const noexcept
{
  const auto &a = _impl->images;
  return {a.pixels, {a.width, a.height}, a.dirty, a.generation, 4};
}

void text_engine_t::clear_dirty() noexcept
{
  _impl->dirty = {};
  _impl->images.dirty = {};
}

std::vector<shaped_glyph_t> text_engine_t::shape(const font_t &font, float pixels, std::string_view text)
{
  std::vector<shaped_glyph_t> out;
  if (!font)
    return out;
  auto *hb = _impl->sized_font(font, std::int32_t(std::lround(pixels * 64.0f)));
  auto *buffer = _impl->buffer;
  hb_buffer_reset(buffer);
  hb_buffer_add_utf8(buffer, text.data(), int(text.size()), 0, int(text.size()));
  hb_buffer_guess_segment_properties(buffer);
  hb_shape(hb, buffer, nullptr, 0);
  unsigned count = 0;
  const auto *infos = hb_buffer_get_glyph_infos(buffer, &count);
  const auto *positions = hb_buffer_get_glyph_positions(buffer, &count);
  std::int64_t pen = 0;
  for (unsigned i = 0; i < count; ++i)
  {
    out.push_back({infos[i].codepoint, float(pen + positions[i].x_offset) / 64.0f, -float(positions[i].y_offset) / 64.0f});
    pen += positions[i].x_advance;
  }
  return out;
}

std::uint32_t text_engine_t::nominal_glyph(const font_t &font, char32_t codepoint)
{
  if (!font)
    return 0;
  auto *hb = hb_font_create(font.data()->face);
  hb_codepoint_t glyph = 0;
  hb_font_get_nominal_glyph(hb, codepoint, &glyph);
  hb_font_destroy(hb);
  return glyph;
}
} // namespace detail
} // namespace vui::ui
