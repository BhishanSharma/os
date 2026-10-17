// libcheck: the libraries of the libs image (toolchain/libs), one check each.
// Prints "ok" or "FAILED" per line; the exit code is the number of failures.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <harfbuzz/hb-ft.h>
#include <harfbuzz/hb.h>
#include <jpeglib.h>
#include <nghttp2/nghttp2.h>
#include <png.h>
#include <unicode/brkiter.h>
#include <unicode/uchar.h>
#include <unicode/unistr.h>
#include <unicode/putil.h>
#include <unicode/uversion.h>
#include <zlib.h>

static int failures;

static void check(const char *what, bool ok, const std::string &detail = "") {
    std::printf("%s  %s%s%s\n", ok ? "ok    " : "FAILED", what, detail.empty() ? "" : ": ", detail.c_str());
    if (!ok) failures++;
}

static const char *find_font() {
    static const char *const paths[] = {"/SYS/INTER.TTF", "/INTER.TTF"};
    for (const char *p : paths)
        if (FILE *f = std::fopen(p, "rb")) {
            std::fclose(f);
            return p;
        }
    return nullptr;
}

int main() {
    std::printf("libcheck: zlib, libpng, libjpeg-turbo, FreeType, HarfBuzz, ICU, nghttp2, curl\n");

    // zlib: compress and back
    std::string text(4000, 'a');
    for (size_t i = 0; i < text.size(); i++) text[i] = (char)('a' + i % 7);
    std::vector<unsigned char> packed(compressBound(text.size()));
    uLongf plen = packed.size();
    compress(packed.data(), &plen, (const Bytef *)text.data(), text.size());
    std::string back(text.size(), 0);
    uLongf blen = back.size();
    int zr = uncompress((Bytef *)back.data(), &blen, packed.data(), plen);
    check("zlib round trip", zr == Z_OK && back == text,
          std::string(zlibVersion()) + ", 4000 -> " + std::to_string(plen) + " bytes");

    // libpng: write a 16x16 RGBA picture to memory, read it back
    std::vector<unsigned char> png_bytes;
    {
        png_image img;
        std::memset(&img, 0, sizeof(img));
        img.version = PNG_IMAGE_VERSION;
        img.width = img.height = 16;
        img.format = PNG_FORMAT_RGBA;
        std::vector<unsigned char> px(16 * 16 * 4);
        for (size_t i = 0; i < px.size(); i++) px[i] = (unsigned char)(i * 3);
        png_alloc_size_t size = 0;
        png_image_write_to_memory(&img, nullptr, &size, 0, px.data(), 0, nullptr);
        png_bytes.resize(size);
        bool wrote = png_image_write_to_memory(&img, png_bytes.data(), &size, 0, px.data(), 0, nullptr);
        png_image in;
        std::memset(&in, 0, sizeof(in));
        in.version = PNG_IMAGE_VERSION;
        bool read = png_image_begin_read_from_memory(&in, png_bytes.data(), size);
        std::vector<unsigned char> out(16 * 16 * 4);
        in.format = PNG_FORMAT_RGBA;
        read = read && png_image_finish_read(&in, nullptr, out.data(), 0, nullptr);
        check("libpng write + read", wrote && read && out == px, std::string(PNG_LIBPNG_VER_STRING));
    }

    // libjpeg-turbo: compress a gradient, decompress it
    {
        jpeg_compress_struct c;
        jpeg_error_mgr err;
        c.err = jpeg_std_error(&err);
        jpeg_create_compress(&c);
        unsigned char *jpg = nullptr;
        unsigned long jlen = 0;
        jpeg_mem_dest(&c, &jpg, &jlen);
        c.image_width = 64;
        c.image_height = 64;
        c.input_components = 3;
        c.in_color_space = JCS_RGB;
        jpeg_set_defaults(&c);
        jpeg_start_compress(&c, TRUE);
        std::vector<unsigned char> row(64 * 3);
        while (c.next_scanline < c.image_height) {
            for (int x = 0; x < 64; x++) row[x * 3] = row[x * 3 + 1] = row[x * 3 + 2] = (unsigned char)(x * 4);
            JSAMPROW r = row.data();
            jpeg_write_scanlines(&c, &r, 1);
        }
        jpeg_finish_compress(&c);
        jpeg_destroy_compress(&c);
        jpeg_decompress_struct d;
        d.err = jpeg_std_error(&err);
        jpeg_create_decompress(&d);
        jpeg_mem_src(&d, jpg, jlen);
        jpeg_read_header(&d, TRUE);
        jpeg_start_decompress(&d);
        bool ok = d.output_width == 64 && d.output_height == 64;
        while (d.output_scanline < d.output_height) {
            JSAMPROW r = row.data();
            jpeg_read_scanlines(&d, &r, 1);
        }
        ok = ok && row[63 * 3] > 200;                   // the bright end of the gradient
        jpeg_finish_decompress(&d);
        jpeg_destroy_decompress(&d);
        free(jpg);
        check("libjpeg-turbo encode + decode", ok, std::to_string(jlen) + " bytes");
    }

    // FreeType + HarfBuzz: load Inter, render a glyph, shape a word
    const char *font = find_font();
    FT_Library ft;
    FT_Face face = nullptr;
    bool ft_ok = FT_Init_FreeType(&ft) == 0 && font && FT_New_Face(ft, font, 0, &face) == 0;
    if (ft_ok) {
        FT_Set_Pixel_Sizes(face, 0, 32);
        ft_ok = FT_Load_Char(face, 'g', FT_LOAD_RENDER) == 0 && face->glyph->bitmap.width > 0;
        check("FreeType: render 'g' at 32 px", ft_ok,
              std::string(face->family_name) + ", " + std::to_string(face->glyph->bitmap.width) + "x" +
                  std::to_string(face->glyph->bitmap.rows));
        hb_font_t *hf = hb_ft_font_create_referenced(face);
        hb_buffer_t *buf = hb_buffer_create();
        hb_buffer_add_utf8(buf, "office", -1, 0, -1);
        hb_buffer_guess_segment_properties(buf);
        hb_shape(hf, buf, nullptr, 0);
        unsigned n = hb_buffer_get_length(buf);
        check("HarfBuzz: shape \"office\"", n >= 4 && n <= 6,
              std::string(hb_version_string()) + ", " + std::to_string(n) + " glyphs (ligatures: fewer than 6)");
        hb_buffer_destroy(buf);
        hb_font_destroy(hf);
    } else {
        check("FreeType: load INTER.TTF", false, font ? "FT_New_Face failed" : "INTER.TTF not found");
    }

    // ICU: case mapping, character properties, word boundaries. Its data is
    // icudt78l.dat in the system folder.
    {
        FILE *dat = std::fopen("/SYS/ICUDT78L.DAT", "rb");
        if (dat) std::fclose(dat);
        u_setDataDirectory(dat ? "/SYS" : "/");
        icu::UnicodeString s = icu::UnicodeString::fromUTF8("straße καλημέρα");
        icu::UnicodeString up = s;
        up.toUpper();
        std::string out;
        up.toUTF8String(out);
        UErrorCode status = U_ZERO_ERROR;
        std::unique_ptr<icu::BreakIterator> words(icu::BreakIterator::createWordInstance(icu::Locale::getUS(), status));
        int count = 0;
        if (U_SUCCESS(status)) {
            words->setText(s);
            for (int32_t p = words->first(); p != icu::BreakIterator::DONE; p = words->next()) count++;
        }
        check("ICU: upper case + word breaks", out == "STRASSE ΚΑΛΗΜΈΡΑ" && count == 4 && u_isalpha(0x3B1),
              std::string("ICU ") + U_ICU_VERSION + ", " + out + ", " + std::to_string(count) + " boundaries");
    }

    // nghttp2 and curl's features
    check("nghttp2", nghttp2_version(0) != nullptr, nghttp2_version(0)->version_str);
    curl_version_info_data *cv = curl_version_info(CURLVERSION_NOW);
    bool h2 = cv->features & CURL_VERSION_HTTP2, gz = cv->features & CURL_VERSION_LIBZ;
    check("curl with HTTP/2 + gzip", h2 && gz, std::string(cv->version) + " " + cv->ssl_version);

    std::printf("libcheck: %s\n", failures ? "FAILED" : "all passed");
    return failures;
}
