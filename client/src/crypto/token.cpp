#include "token.h"

#include <QRandomGenerator>

namespace fb {

std::vector<uint8_t> random_bytes(size_t n) {
    std::vector<uint8_t> v((n + 3) & ~size_t(3));
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(v.data()),
                                          static_cast<qsizetype>(v.size() / 4));
    v.resize(n);
    return v;
}

} // namespace fb
