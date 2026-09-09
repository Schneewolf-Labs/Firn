#include "firn/print.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

#include "firn/io.h"
#include "firn/raster.h"

namespace firn::print {

bool write_pdf(const Image& img, const PageSetup& setup, const std::string& path, std::string* err) {
    if (img.empty()) { if (err) *err = "empty image"; return false; }
    // Flatten onto white and JPEG-encode through the image writer.
    Image flat(img.width(), img.height(), {255, 255, 255, 255});
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) raster::blend_over(flat, x, y, img.get(x, y), 1.0f);
    const std::string tmp = path + ".firn-print.jpg";
    if (!io::save(flat, tmp, err, std::clamp(setup.jpeg_quality, 1, 100))) return false;
    std::ifstream jf(tmp, std::ios::binary);
    std::vector<char> jpeg((std::istreambuf_iterator<char>(jf)), std::istreambuf_iterator<char>());
    jf.close();
    std::remove(tmp.c_str());
    if (jpeg.empty()) { if (err) *err = "JPEG encoding failed"; return false; }

    const float pw = (setup.landscape ? setup.page_h_in : setup.page_w_in) * 72.0f;
    const float ph = (setup.landscape ? setup.page_w_in : setup.page_h_in) * 72.0f;
    const float margin = std::max(0.0f, setup.margin_in) * 72.0f;
    const float avail_w = std::max(1.0f, pw - 2 * margin), avail_h = std::max(1.0f, ph - 2 * margin);
    float w, h;
    if (setup.scale_percent > 0.0f) {
        w = img.width() / static_cast<float>(std::max(1, setup.dpi)) * 72.0f * setup.scale_percent / 100.0f;
        h = img.height() / static_cast<float>(std::max(1, setup.dpi)) * 72.0f * setup.scale_percent / 100.0f;
    } else {
        const float s = std::min(avail_w / img.width(), avail_h / img.height());
        w = img.width() * s; h = img.height() * s;
    }
    const float x = setup.center ? (pw - w) * 0.5f : margin;
    const float y = setup.center ? (ph - h) * 0.5f : ph - margin - h;

    std::ostringstream content;
    content << "q " << w << " 0 0 " << h << " " << x << " " << y << " cm /Im0 Do Q\n";
    const std::string cs = content.str();

    std::ofstream f(path, std::ios::binary);
    if (!f) { if (err) *err = "cannot write " + path; return false; }
    std::vector<std::streamoff> offsets;
    auto obj = [&](const std::string& body) { offsets.push_back(f.tellp()); f << offsets.size() << " 0 obj\n" << body << "\nendobj\n"; };
    f << "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    obj("<< /Type /Catalog /Pages 2 0 R >>");
    obj("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    {
        std::ostringstream o;
        o << "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " << pw << " " << ph << "] /Contents 4 0 R /Resources << /XObject << /Im0 5 0 R >> >> >>";
        obj(o.str());
    }
    {
        std::ostringstream o;
        o << "<< /Length " << cs.size() << " >>\nstream\n" << cs << "endstream";
        obj(o.str());
    }
    {
        offsets.push_back(f.tellp());
        f << "5 0 obj\n<< /Type /XObject /Subtype /Image /Width " << img.width() << " /Height " << img.height()
          << " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length " << jpeg.size() << " >>\nstream\n";
        f.write(jpeg.data(), static_cast<std::streamsize>(jpeg.size()));
        f << "\nendstream\nendobj\n";
    }
    {
        std::ostringstream o;
        std::string title = setup.title;
        for (char& c : title) if (c == '(' || c == ')' || c == '\\') c = ' ';
        o << "<< /Producer (Firn) /Title (" << title << ") >>";
        obj(o.str());
    }
    const std::streamoff xref = f.tellp();
    f << "xref\n0 " << offsets.size() + 1 << "\n0000000000 65535 f \n";
    for (std::streamoff off : offsets) { char line[24]; std::snprintf(line, sizeof(line), "%010lld 00000 n \n", static_cast<long long>(off)); f << line; }
    f << "trailer\n<< /Size " << offsets.size() + 1 << " /Root 1 0 R /Info " << offsets.size() << " 0 R >>\nstartxref\n" << xref << "\n%%EOF\n";
    return static_cast<bool>(f);
}

bool send_to_printer(const std::string& pdf_path, const std::string& printer, std::string* err) {
#ifdef _WIN32
    (void)pdf_path; (void)printer;
    if (err) *err = "printing goes through the saved PDF on Windows";
    return false;
#else
    std::string cmd = "lp";
    if (!printer.empty()) cmd += " -d '" + printer + "'";
    cmd += " '" + pdf_path + "' >/dev/null 2>&1";
    const int rc = std::system(cmd.c_str());
    if (rc != 0) { if (err) *err = "lp failed (is CUPS installed and a printer configured?)"; return false; }
    return true;
#endif
}

}  // namespace firn::print
