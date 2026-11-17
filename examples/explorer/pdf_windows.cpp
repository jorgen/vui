#if defined(_WIN32)

// PDF pages on Windows (G61): Windows.Data.Pdf, the renderer Windows 10 and 11 carry, through
// C++/WinRT from the SDK. Kept out of shell_windows.cpp because the WinRT headers are heavy.

#include "shell.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <future>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <objbase.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

namespace shell
{
namespace
{
std::expected<pdf_page_t, std::string> render(const std::string &path, int page, int pixels)
{
  using namespace winrt::Windows;
  try
  {
    const auto file = Storage::StorageFile::GetFileFromPathAsync(winrt::to_hstring(path)).get();
    const auto document = Data::Pdf::PdfDocument::LoadFromFileAsync(file).get();
    pdf_page_t out;
    out.pages = int(document.PageCount());
    if (out.pages <= 0)
      return std::unexpected(std::string("the PDF has no pages"));
    out.page = std::clamp(page, 0, out.pages - 1);
    const auto pdf_page = document.GetPage(std::uint32_t(out.page));
    const auto size = pdf_page.Size();
    const double longer = std::max<double>(std::max(size.Width, size.Height), 1);
    const double scale = double(pixels) / longer;
    const auto wanted_w = std::max(1, int(std::lround(size.Width * scale)));
    const auto wanted_h = std::max(1, int(std::lround(size.Height * scale)));
    const auto draw = [&](double dips_per_pixel)
    {
      Data::Pdf::PdfPageRenderOptions options;
      options.DestinationWidth(std::max<std::uint32_t>(1, std::uint32_t(std::lround(wanted_w * dips_per_pixel))));
      options.DestinationHeight(std::max<std::uint32_t>(1, std::uint32_t(std::lround(wanted_h * dips_per_pixel))));
      Storage::Streams::InMemoryRandomAccessStream stream;
      pdf_page.RenderToStreamAsync(stream, options).get();
      stream.Seek(0);
      const auto decoder = Graphics::Imaging::BitmapDecoder::CreateAsync(stream).get();
      const auto data = decoder
                          .GetPixelDataAsync(Graphics::Imaging::BitmapPixelFormat::Rgba8, Graphics::Imaging::BitmapAlphaMode::Straight, Graphics::Imaging::BitmapTransform(),
                                             Graphics::Imaging::ExifOrientationMode::IgnoreExifOrientation, Graphics::Imaging::ColorManagementMode::DoNotColorManage)
                          .get();
      const auto bytes = data.DetachPixelData();
      out.picture.width = int(decoder.PixelWidth());
      out.picture.height = int(decoder.PixelHeight());
      out.picture.rgba.assign(bytes.begin(), bytes.end());
    };
    // The destination size is in device-independent units, which the renderer multiplies by the
    // display's scale; how much is found from the first page drawn, and drawn again if it was off.
    static std::atomic<double> dips_per_pixel{0};
    if (const double known = dips_per_pixel.load(); known > 0)
      draw(known);
    else
    {
      draw(1);
      const double measured = double(wanted_w) / std::max(1, out.picture.width);
      dips_per_pixel.store(measured);
      if (std::abs(out.picture.width - wanted_w) > 1)
        draw(measured);
    }
    if (out.picture.rgba.size() != std::size_t(out.picture.width) * out.picture.height * 4)
      return std::unexpected(std::string("the page could not be read back"));
    return out;
  }
  catch (const winrt::hresult_error &e)
  {
    return std::unexpected(winrt::to_string(e.message()));
  }
}
} // namespace

std::expected<pdf_page_t, std::string> pdf_page(const std::string &path, int page, int pixels)
{
  // On a thread of its own in the multithreaded apartment: the caller's is single-threaded for the
  // shell, and C++/WinRT will not block on an operation there. A thread is cheap next to a page.
  return std::async(std::launch::async,
                    [&]
                    {
                      winrt::init_apartment(winrt::apartment_type::multi_threaded);
                      auto r = render(path, page, pixels);
                      // Windows.Data.Pdf keeps a Direct3D device, and left loaded it releases that
                      // in its own teardown at process exit, where Direct3D fails fast. So nothing
                      // of it is kept: the factories C++/WinRT cached go, then the DLL itself, now.
                      winrt::clear_factory_cache();
                      CoFreeUnusedLibrariesEx(0, 0);
                      winrt::uninit_apartment();
                      return r;
                    })
    .get();
}
} // namespace shell

#endif
