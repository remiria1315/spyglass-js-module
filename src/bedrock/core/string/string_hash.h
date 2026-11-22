#pragma once

#include <cstdint>
#include <string>

using HashType64 = std::uint64_t;

class HashedString {
public:
    [[nodiscard]] const std::string &getString() const { return mStr; }

private:
    HashType64 mStrHash{0};
    std::string mStr;
    const HashedString *mLastMatch{nullptr};
};

static_assert(sizeof(HashedString) == 48);
