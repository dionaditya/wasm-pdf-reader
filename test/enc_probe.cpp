// enc_probe.cpp — print the file key this engine derives for a fixture.
//
//   g++ -O1 -I src/cpp test/enc_probe.cpp src/cpp/pdfdoc.cpp src/cpp/cfffont.cpp \
//       src/cpp/cff-parser.cc src/cpp/cmap.cpp src/cpp/cmaps_data.cpp -lz -o /tmp/enc_probe
//   /tmp/enc_probe test/fixtures/enc-r6-aes256.pdf owner-fixture
//   LD_LIBRARY_PATH=/tmp/qpdfbin/lib /tmp/qpdfbin/bin/qpdf --password=owner-fixture \
//       --show-encryption-key test/fixtures/enc-r6-aes256.pdf
//
// The two keys must be byte-identical: that is the difference between "opens the
// file" and "derives the same key the reference implementation does".

#include <cstdio>
#include <string>
#include <vector>

#include "pdfdoc.hpp"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <file.pdf> <password>\n", argv[0]);
        return 2;
    }
    std::FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::perror("open"); return 2; }
    std::vector<unsigned char> bytes;
    unsigned char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);

    pdf::Document doc;
    doc.password = argv[2];
    const bool ok = doc.load(bytes.data(), bytes.size());

    std::printf("%-34s pw=%-14s ok=%d enc=%d direct=%d aes=%d keylen=%2d key=",
                argv[1], argv[2][0] ? argv[2] : "(empty)", ok ? 1 : 0,
                doc.encrypted ? 1 : 0, doc.encDirectKey ? 1 : 0, doc.aesStreams ? 1 : 0,
                doc.encN);
    for (unsigned char c : doc.encKey) std::printf("%02x", c);
    if (!ok) std::printf("  error=%s", doc.error.c_str());
    if (!doc.warnings.empty()) std::printf("  warn=%s", doc.warnings.front().c_str());
    std::printf("\n");
    return ok ? 0 : 1;
}
