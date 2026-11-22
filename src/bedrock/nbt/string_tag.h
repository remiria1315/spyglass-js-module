#pragma once

#include <string>

#include "bedrock/nbt/tag.h"

class StringTag : public Tag {
public:
    std::string data;
};

static_assert(sizeof(StringTag) == 40);
