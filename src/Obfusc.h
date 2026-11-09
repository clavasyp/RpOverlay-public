#pragma once

#include <QString>
#include <cstddef>

// Лёгкая компайл-тайм XOR-обфускация строковых литералов (Слой B1 плана усиления защиты).
//
// Цель — чтобы чувствительные литералы (анти-VM фильтры HWID, relay-IP) не лежали ОТКРЫТЫМ ASCII в
// .rodata на промежуточных стадиях анализа. Это «поднять планку дампу строк», НЕ криптостойкость:
// настоящее шифрование строк даёт Themida (STR_ENCRYPT при упаковке) — здесь лишь дополнительный слой
// до неё. constexpr-шифр кладёт в бинарь уже XOR-байты; рантайм-дешифр идёт через volatile-ключ,
// чтобы -O2 не свернул вычисление обратно в исходный литерал.
namespace obf {

template <std::size_t N>
class Str {
public:
    constexpr Str(const char (&s)[N], unsigned char key) : key_(key), buf_{} {
        for (std::size_t i = 0; i < N; ++i)
            buf_[i] = static_cast<char>(static_cast<unsigned char>(s[i]) ^ key);
    }
    QString str() const {
        char out[N];
        volatile unsigned char k = key_;     // volatile глушит constant-folding обратно в плейнтекст
        for (std::size_t i = 0; i < N; ++i)
            out[i] = static_cast<char>(static_cast<unsigned char>(buf_[i]) ^ k);
        return QString::fromUtf8(out, static_cast<int>(N) - 1);  // без хвостового NUL
    }
private:
    unsigned char key_;
    char          buf_[N];
};

} // namespace obf

// OBF("literal") → QString в рантайме. Ключ выводится из __LINE__ (разные строки — разный байт),
// форсированно нечётный (исключаем вырожденный key=0 = «нет обфускации»).
#define OBF(lit) ([] {                                                                       \
        static constexpr obf::Str<sizeof(lit)> _o(                                           \
            lit, static_cast<unsigned char>(((__LINE__ * 37u + 91u) & 0xFFu) | 1u));         \
        return _o.str();                                                                     \
    }())
