#include <fstream>

#include "sandbox.h"
#include "sha256.h"
#include "test.h"

using namespace vrml;

TEST(nist_vectors) {
    CHECK_EQ(Sha256Hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(Sha256Hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

TEST(million_a_in_pieces) {
    Sha256 s;
    std::string chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i) s.update(chunk.data(), chunk.size());
    CHECK_EQ(s.hex(), std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

TEST(odd_chunking_gives_the_same_hash) {
    std::string data;
    for (int i = 0; i < 1000; ++i) data += static_cast<char>('a' + i % 26);
    Sha256 a, b;
    a.update(data.data(), data.size());
    for (size_t i = 0; i < data.size(); i += 7) b.update(data.data() + i, std::min<size_t>(7, data.size() - i));
    CHECK_EQ(a.hex(), b.hex());
}

TEST(padding_boundaries) {
    // lengths around the block padding edge (55, 56, 63, 64, 65 bytes): hashed in one piece and byte by byte
    for (size_t n : {size_t(0), size_t(1), size_t(55), size_t(56), size_t(63), size_t(64), size_t(65), size_t(119), size_t(120), size_t(128)}) {
        std::string d(n, 'x');
        Sha256 one, two;
        one.update(d.data(), d.size());
        for (char c : d) two.update(&c, 1);
        CHECK_EQ(one.hex(), two.hex());
    }
    CHECK_EQ(Sha256Hex(std::string(55, 'a')), std::string("9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"));
    CHECK_EQ(Sha256Hex(std::string(64, 'a')), std::string("ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"));
}

TEST(file_hash) {
    tt::Sandbox sb;
    tt::Write(sb.root / "f.bin", "abc");
    std::string h;
    CHECK(Sha256File(sb.root / "f.bin", &h));
    CHECK_EQ(h, std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK(!Sha256File(sb.root / "missing.bin", &h));
    CHECK(h.empty());
}

TEST(hex_shape) {
    CHECK(IsSha256Hex("f7bf0287398890b09bcc68b576327e727304bd88f7be80160f9676e8bd6871e2"));
    CHECK(IsSha256Hex("F7BF0287398890B09BCC68B576327E727304BD88F7BE80160F9676E8BD6871E2"));
    CHECK(!IsSha256Hex("f7bf"));
    CHECK(!IsSha256Hex("g7bf0287398890b09bcc68b576327e727304bd88f7be80160f9676e8bd6871e2"));
}

TEST_MAIN()
