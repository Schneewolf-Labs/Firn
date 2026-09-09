#pragma once
#include <string>

#include "firn/image.h"

// Printing: a page description written as PDF (JPEG-compressed image on a
// page), which any system printer can take.
namespace firn::print {

struct PageSetup {
    float page_w_in = 8.5f, page_h_in = 11.0f;   // Letter portrait; A4 = 8.27 x 11.69
    bool landscape = false;
    float margin_in = 0.5f;
    float scale_percent = 0.0f;                  // 0 = fit to the printable area
    int dpi = 300;                               // pixels per inch at 100 %
    bool center = true;
    int jpeg_quality = 92;
    std::string title;
};

// Writes a one-page PDF with the image placed per `setup`.
bool write_pdf(const Image& img, const PageSetup& setup, const std::string& path, std::string* err = nullptr);
// Sends a PDF to the default (or named) printer with the system spooler; false when none is available.
bool send_to_printer(const std::string& pdf_path, const std::string& printer, std::string* err = nullptr);

}  // namespace firn::print
