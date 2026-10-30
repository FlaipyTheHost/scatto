#pragma once

#include <glib/gi18n.h>  // _(), N_(), C_(), NC_()
#include <libintl.h>     // ngettext()

#include <cstdarg>
#include <cstdio>
#include <string>

// printf into a std::string. Translatable sentences that embed values are
// written as whole format strings -- _("Restore %s?") -- so translators can
// reorder the words, instead of being glued together from fragments.
inline std::string format_string(const char* format, ...) __attribute__((format(printf, 1, 2)));

inline std::string format_string(const char* format, ...) {
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    const int length = std::vsnprintf(nullptr, 0, format, copy);
    va_end(copy);

    std::string out(length > 0 ? static_cast<std::size_t>(length) : 0, '\0');
    if (length > 0) std::vsnprintf(&out[0], out.size() + 1, format, args);
    va_end(args);
    return out;
}
