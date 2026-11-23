#pragma once

#include <string>
#include <string_view>

namespace abx {

std::string RoutedLibrary(std::wstring_view requested);
void InstallLoaderRedirect();

}
