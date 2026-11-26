#pragma once

#include <functional>

namespace abx {

void AddBufferRedirects(const std::function<void(const char*, void*)>& add);

}
