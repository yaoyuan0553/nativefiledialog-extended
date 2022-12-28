/*
  Native File Dialog Extended
  Repository: https://github.com/btzy/nativefiledialog-extended
  License: Zlib
  Authors: Bernard Teo

  Note: We do not check for malloc failure on Linux - Linux overcommits memory!
*/

#include <assert.h>
#include <dbus/dbus.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>  // for the random token string
#include <unistd.h>      // for access()
#include <libgen.h>
#include <pthread.h>

#include <cctype>
#include <concepts>
#include <new>
#include <utility>

#include "nfd.h"

/*
Define NFD_APPEND_EXTENSION if you want the file extension to be appended when missing. Linux
programs usually don't append the file extension, but for consistency with other OSes you might want
to append it.  However, when using portals, the file overwrite prompt and the Flatpak sandbox won't
know that we appended an extension, so they will not check or whitelist the correct file.  Enabling
NFD_APPEND_EXTENSION is not recommended for portals.
*/

namespace {

template <typename T = void>
T* NFDi_Malloc(size_t bytes) {
    void* ptr = malloc(bytes);
    assert(ptr);  // Linux malloc never fails

    return static_cast<T*>(ptr);
}

template <typename T = void>
T* NFDi_Realloc(T* ptr, size_t bytes) {
    void* newPtr = realloc(ptr, bytes);
    assert(newPtr);  // Linux malloc never fails

    return static_cast<T*>(newPtr);
}

template <typename T>
void NFDi_Free(T* ptr) {
    free(static_cast<void*>(ptr));
}

template <typename T>
struct Free_Guard {
    T* data;
    Free_Guard(T* freeable) noexcept : data(freeable) {}
    ~Free_Guard() { NFDi_Free(data); }
    T* release() noexcept
    {
        T* tmp = data;
        data = nullptr;
        return tmp;
    }
};

template <typename T>
struct MallocFreeGuard {
    T* data;
    MallocFreeGuard(T* freeable) noexcept : data(freeable) {}
    ~MallocFreeGuard() { free(data); }
    T* release() noexcept
    {
        T* tmp = data;
        data = nullptr;
        return tmp;
    }
};

template <typename T>
struct FreeCheck_Guard {
    T* data;
    FreeCheck_Guard(T* freeable = nullptr) noexcept : data(freeable) {}
    ~FreeCheck_Guard() {
        if (data) NFDi_Free(data);
    }
};

struct DBusMessage_Guard {
    DBusMessage* data;
    DBusMessage_Guard(DBusMessage* freeable) noexcept : data(freeable) {}
    ~DBusMessage_Guard() { dbus_message_unref(data); }
};


/* D-Bus connection handle */
DBusConnection* dbus_conn;
/* current D-Bus error */
DBusError dbus_err;
/* current error (may be a pointer to the D-Bus error message above, or a pointer to some string
 * literal) */
const char* err_ptr = nullptr;
/* the unique name of our connection, used for the Request handle; owned by D-Bus so we don't free
 * it */
const char* dbus_unique_name;


void NFDi_SetError(const char* msg) {
    err_ptr = msg;
}

template <typename T>
T* copy(const T* begin, const T* end, T* out) {
    for (; begin != end; ++begin) {
        *out++ = *begin;
    }
    return out;
}

char* genCaseSensitivePattern(const char* begin, const char* end, char* out) {
    for (; begin != end; ++begin) {
        if (isalpha(*begin)) {
            *out++ = '[';
            *out++ = static_cast<char>(tolower(*begin));
            *out++ = static_cast<char>(toupper(*begin));
            *out++ = ']';
        }
        else
            *out++ = *begin;
    }
    return out;
}

template <typename T, typename Callback>
T* transform(const T* begin, const T* end, T* out, Callback callback) {
    for (; begin != end; ++begin) {
        *out++ = callback(*begin);
    }
    return out;
}

constexpr const char* STR_EMPTY = "";
constexpr const char* STR_OPEN_FILE = "Open File";
constexpr const char* STR_OPEN_FILES = "Open Files";
constexpr const char* STR_SAVE_FILE = "Save File";
constexpr const char* STR_SELECT_FOLDER = "Select Folder";
constexpr const char* STR_HANDLE_TOKEN = "handle_token";
constexpr const char* STR_MULTIPLE = "multiple";
constexpr const char* STR_DIRECTORY = "directory";
constexpr const char* STR_FILTERS = "filters";
constexpr const char* STR_CURRENT_FILTER = "current_filter";
constexpr const char* STR_CURRENT_NAME = "current_name";
constexpr const char* STR_CURRENT_FOLDER = "current_folder";
constexpr const char* STR_CURRENT_FILE = "current_file";
constexpr const char* STR_ALL_FILES = "All files";
constexpr const char* STR_ASTERISK = "*";

template <bool Multiple, bool Directory>
void AppendOpenFileQueryTitle(DBusMessageIter& iter, const char* title = nullptr)
{
    if (title)
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &title);
    else {
        if constexpr (!Multiple && !Directory)
            dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &STR_OPEN_FILE);
        else if constexpr (Multiple && !Directory)
            dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &STR_OPEN_FILES);
        else if constexpr (!Multiple)
            dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &STR_SELECT_FOLDER);
    }
}

//template <>
//void AppendOpenFileQueryTitle<false, false>(DBusMessageIter& iter) {
//    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &STR_OPEN_FILE);
//}
//template <>
//void AppendOpenFileQueryTitle<true, false>(DBusMessageIter& iter) {
//    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &STR_OPEN_FILES);
//}
//template <>
//void AppendOpenFileQueryTitle<false, true>(DBusMessageIter& iter) {
//    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &STR_SELECT_FOLDER);
//}

void AppendSaveFileQueryTitle(DBusMessageIter& iter, const char* title = STR_SAVE_FILE) {
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &title);
}

void AppendOpenFileQueryDictEntryHandleToken(DBusMessageIter& sub_iter, const char* handle_token) {
    DBusMessageIter sub_sub_iter;
    DBusMessageIter variant_iter;
    dbus_message_iter_open_container(&sub_iter, DBUS_TYPE_DICT_ENTRY, nullptr, &sub_sub_iter);
    dbus_message_iter_append_basic(&sub_sub_iter, DBUS_TYPE_STRING, &STR_HANDLE_TOKEN);
    dbus_message_iter_open_container(&sub_sub_iter, DBUS_TYPE_VARIANT, "s", &variant_iter);
    dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_STRING, &handle_token);
    dbus_message_iter_close_container(&sub_sub_iter, &variant_iter);
    dbus_message_iter_close_container(&sub_iter, &sub_sub_iter);
}

template <bool Multiple>
void AppendOpenFileQueryDictEntryMultiple(DBusMessageIter&);
template <>
void AppendOpenFileQueryDictEntryMultiple<true>(DBusMessageIter& sub_iter) {
    DBusMessageIter sub_sub_iter;
    DBusMessageIter variant_iter;
    dbus_message_iter_open_container(&sub_iter, DBUS_TYPE_DICT_ENTRY, nullptr, &sub_sub_iter);
    dbus_message_iter_append_basic(&sub_sub_iter, DBUS_TYPE_STRING, &STR_MULTIPLE);
    dbus_message_iter_open_container(&sub_sub_iter, DBUS_TYPE_VARIANT, "b", &variant_iter);
    {
        u_int32_t b = true;
        dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_BOOLEAN, &b);
    }
    dbus_message_iter_close_container(&sub_sub_iter, &variant_iter);
    dbus_message_iter_close_container(&sub_iter, &sub_sub_iter);
}
template <>
void AppendOpenFileQueryDictEntryMultiple<false>(DBusMessageIter&) {}

template <bool Directory>
void AppendOpenFileQueryDictEntryDirectory(DBusMessageIter&);
template <>
void AppendOpenFileQueryDictEntryDirectory<true>(DBusMessageIter& sub_iter) {
    DBusMessageIter sub_sub_iter;
    DBusMessageIter variant_iter;
    dbus_message_iter_open_container(&sub_iter, DBUS_TYPE_DICT_ENTRY, nullptr, &sub_sub_iter);
    dbus_message_iter_append_basic(&sub_sub_iter, DBUS_TYPE_STRING, &STR_DIRECTORY);
    dbus_message_iter_open_container(&sub_sub_iter, DBUS_TYPE_VARIANT, "b", &variant_iter);
    {
        int b = true;
        dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_BOOLEAN, &b);
    }
    dbus_message_iter_close_container(&sub_sub_iter, &variant_iter);
    dbus_message_iter_close_container(&sub_iter, &sub_sub_iter);
}
template <>
void AppendOpenFileQueryDictEntryDirectory<false>(DBusMessageIter&) {}

void AppendSingleFilter(DBusMessageIter& base_iter, const nfdnfilteritem_t& filter) {
    DBusMessageIter filter_list_struct_iter;
    DBusMessageIter filter_sublist_iter;
    DBusMessageIter filter_sublist_struct_iter;
    dbus_message_iter_open_container(
        &base_iter, DBUS_TYPE_STRUCT, nullptr, &filter_list_struct_iter);
    // count number of file extensions
    size_t sep = 1;
    for (const char* p = filter.spec; *p; ++p) {
        if (*p == L',') {
            ++sep;
        }
    }
    {
        const size_t name_len = strlen(filter.name);
        const size_t spec_len = strlen(filter.spec);
        char* buf = static_cast<char*>(alloca(sep + name_len + 2 + spec_len + 1));
        char* buf_end = buf;
        buf_end = copy(filter.name, filter.name + name_len, buf_end);
        *buf_end++ = ' ';
        *buf_end++ = '(';
        const char* spec_ptr = filter.spec;
        do {
            *buf_end++ = *spec_ptr;
            if (*spec_ptr == ',') *buf_end++ = ' ';
            ++spec_ptr;
        } while (*spec_ptr != '\0');
        *buf_end++ = ')';
        *buf_end = '\0';
        dbus_message_iter_append_basic(&filter_list_struct_iter, DBUS_TYPE_STRING, &buf);
    }
    {
        dbus_message_iter_open_container(
            &filter_list_struct_iter, DBUS_TYPE_ARRAY, "(us)", &filter_sublist_iter);
        const char* extn_begin = filter.spec;
        const char* extn_end = extn_begin;
        while (true) {
            dbus_message_iter_open_container(
                &filter_sublist_iter, DBUS_TYPE_STRUCT, nullptr, &filter_sublist_struct_iter);
            {
                const unsigned zero = 0;
                dbus_message_iter_append_basic(
                    &filter_sublist_struct_iter, DBUS_TYPE_UINT32, &zero);
            }
            do {
                ++extn_end;
            } while (*extn_end != ',' && *extn_end != '\0');
            char* buf = static_cast<char*>(alloca(2 + (extn_end - extn_begin) + 1));
            char* buf_end = buf;
            *buf_end++ = '*';
            *buf_end++ = '.';
            buf_end = copy(extn_begin, extn_end, buf_end);
            *buf_end = '\0';
            dbus_message_iter_append_basic(&filter_sublist_struct_iter, DBUS_TYPE_STRING, &buf);
            dbus_message_iter_close_container(&filter_sublist_iter, &filter_sublist_struct_iter);
            if (*extn_end == '\0') {
                break;
            }
            extn_begin = extn_end + 1;
            extn_end = extn_begin;
        }
    }
    dbus_message_iter_close_container(&filter_list_struct_iter, &filter_sublist_iter);
    dbus_message_iter_close_container(&base_iter, &filter_list_struct_iter);
}

void AppendSingleFilter(DBusMessageIter& base_iter, const char* name, const char* pattern)
{
    DBusMessageIter filter_list_struct_iter;
    DBusMessageIter filter_sublist_iter;
    DBusMessageIter filter_sublist_struct_iter;
    dbus_message_iter_open_container(
        &base_iter, DBUS_TYPE_STRUCT, nullptr, &filter_list_struct_iter);
    // count number of file extensions
    size_t sep = 1;
    for (const char* p = pattern; *p; ++p) {
        if (*p == ';') {
            ++sep;
        }
    }
    {
//        const size_t name_len = strlen(name);
//        const size_t pattern_len = strlen(pattern);
//        char* buf = static_cast<char*>(alloca(sep + name_len + 2 + pattern_len + 1));
//        char* buf_end = buf;
//        buf_end = copy(name, name + name_len, buf_end);
//        *buf_end++ = ' ';
//        *buf_end++ = '(';
//        const char* pattern_ptr = pattern;
//        do {
//            *buf_end++ = *pattern_ptr;
//            if (*pattern_ptr == ';') *buf_end++ = ' ';
//            ++pattern_ptr;
//        } while (*pattern_ptr != '\0');
//        *buf_end++ = ')';
//        *buf_end = '\0';
        dbus_message_iter_append_basic(&filter_list_struct_iter, DBUS_TYPE_STRING, &name);
    }
    {
        dbus_message_iter_open_container(
            &filter_list_struct_iter, DBUS_TYPE_ARRAY, "(us)", &filter_sublist_iter);
        const char* pattern_begin = pattern;
        const char* pattern_end = pattern_begin;
        while (true) {
            dbus_message_iter_open_container(
                &filter_sublist_iter, DBUS_TYPE_STRUCT, nullptr, &filter_sublist_struct_iter);
            {
                const unsigned zero = 0;
                dbus_message_iter_append_basic(
                    &filter_sublist_struct_iter, DBUS_TYPE_UINT32, &zero);
            }
            // TODO: ignore case
            int letter_count = 0;
            do {
                if (isalpha(*pattern_end))
                    ++letter_count;
                ++pattern_end;
            } while (*pattern_end != ';' && *pattern_end != '\0');
            char* buf =
                static_cast<char*>(alloca((pattern_end - pattern_begin) + 3 * letter_count + 1));
            char* buf_end = buf;
//            buf_end = copy(pattern_begin, pattern_end, buf_end);
            buf_end = genCaseSensitivePattern(pattern_begin, pattern_end, buf_end);
            *buf_end = '\0';
            fprintf(stderr, "appending filter %s\n", buf);
            dbus_message_iter_append_basic(&filter_sublist_struct_iter, DBUS_TYPE_STRING, &buf);
            dbus_message_iter_close_container(&filter_sublist_iter, &filter_sublist_struct_iter);
            if (*pattern_end == '\0') {
                break;
            }
            pattern_begin = pattern_end + 1;
            pattern_end = pattern_begin;
        }
    }
    dbus_message_iter_close_container(&filter_list_struct_iter, &filter_sublist_iter);
    dbus_message_iter_close_container(&base_iter, &filter_list_struct_iter);
}

bool AppendSingleFilterCheckExtn(DBusMessageIter& base_iter,
                                 const nfdnfilteritem_t& filter,
                                 const nfdnchar_t* match_extn) {
    DBusMessageIter filter_list_struct_iter;
    DBusMessageIter filter_sublist_iter;
    DBusMessageIter filter_sublist_struct_iter;
    dbus_message_iter_open_container(
        &base_iter, DBUS_TYPE_STRUCT, nullptr, &filter_list_struct_iter);
    // count number of file extensions
    size_t sep = 1;
    for (const char* p = filter.spec; *p; ++p) {
        if (*p == L',') {
            ++sep;
        }
    }
    {
        const size_t name_len = strlen(filter.name);
        const size_t spec_len = strlen(filter.spec);
        char* buf = static_cast<char*>(alloca(sep + name_len + 2 + spec_len + 1));
        char* buf_end = buf;
        buf_end = copy(filter.name, filter.name + name_len, buf_end);
        *buf_end++ = ' ';
        *buf_end++ = '(';
        const char* spec_ptr = filter.spec;
        do {
            *buf_end++ = *spec_ptr;
            if (*spec_ptr == ',') *buf_end++ = ' ';
            ++spec_ptr;
        } while (*spec_ptr != '\0');
        *buf_end++ = ')';
        *buf_end = '\0';
        dbus_message_iter_append_basic(&filter_list_struct_iter, DBUS_TYPE_STRING, &buf);
    }
    bool extn_matched = false;
    {
        dbus_message_iter_open_container(
            &filter_list_struct_iter, DBUS_TYPE_ARRAY, "(us)", &filter_sublist_iter);
        const char* extn_begin = filter.spec;
        const char* extn_end = extn_begin;
        while (true) {
            dbus_message_iter_open_container(
                &filter_sublist_iter, DBUS_TYPE_STRUCT, nullptr, &filter_sublist_struct_iter);
            {
                const unsigned zero = 0;
                dbus_message_iter_append_basic(
                    &filter_sublist_struct_iter, DBUS_TYPE_UINT32, &zero);
            }
            do {
                ++extn_end;
            } while (*extn_end != ',' && *extn_end != '\0');
            char* buf = static_cast<char*>(alloca(2 + (extn_end - extn_begin) + 1));
            char* buf_end = buf;
            *buf_end++ = '*';
            *buf_end++ = '.';
            buf_end = copy(extn_begin, extn_end, buf_end);
            *buf_end = '\0';
            dbus_message_iter_append_basic(&filter_sublist_struct_iter, DBUS_TYPE_STRING, &buf);
            dbus_message_iter_close_container(&filter_sublist_iter, &filter_sublist_struct_iter);
            if (!extn_matched) {
                const char* match_extn_p;
                const char* p;
                for (p = extn_begin, match_extn_p = match_extn; p != extn_end && *match_extn_p;
                     ++p, ++match_extn_p) {
                    if (*p != *match_extn_p) break;
                }
                if (p == extn_end && !*match_extn_p) {
                    extn_matched = true;
                }
            }
            if (*extn_end == '\0') {
                break;
            }
            extn_begin = extn_end + 1;
            extn_end = extn_begin;
        }
    }
    dbus_message_iter_close_container(&filter_list_struct_iter, &filter_sublist_iter);
    dbus_message_iter_close_container(&base_iter, &filter_list_struct_iter);
    return extn_matched;
}

void AppendWildcardFilter(DBusMessageIter& base_iter, const char* name = nullptr) {
    DBusMessageIter filter_list_struct_iter;
    DBusMessageIter filter_sublist_iter;
    DBusMessageIter filter_sublist_struct_iter;
    dbus_message_iter_open_container(
        &base_iter, DBUS_TYPE_STRUCT, nullptr, &filter_list_struct_iter);
    if (name)
        dbus_message_iter_append_basic(&filter_list_struct_iter, DBUS_TYPE_STRING, &name);
    else
        dbus_message_iter_append_basic(&filter_list_struct_iter, DBUS_TYPE_STRING, &STR_ALL_FILES);
    dbus_message_iter_open_container(
        &filter_list_struct_iter, DBUS_TYPE_ARRAY, "(us)", &filter_sublist_iter);
    dbus_message_iter_open_container(
        &filter_sublist_iter, DBUS_TYPE_STRUCT, nullptr, &filter_sublist_struct_iter);
    {
        const unsigned zero = 0;
        dbus_message_iter_append_basic(&filter_sublist_struct_iter, DBUS_TYPE_UINT32, &zero);
    }
    dbus_message_iter_append_basic(&filter_sublist_struct_iter, DBUS_TYPE_STRING, &STR_ASTERISK);
    dbus_message_iter_close_container(&filter_sublist_iter, &filter_sublist_struct_iter);
    dbus_message_iter_close_container(&filter_list_struct_iter, &filter_sublist_iter);
    dbus_message_iter_close_container(&base_iter, &filter_list_struct_iter);
}

void AppendFileQueryDictEntryFilters(DBusMessageIter& sub_iter,
                                     const char* winFilter,
                                     unsigned long filterIndex)
{
    if (winFilter && *winFilter != '\0') {
        DBusMessageIter sub_sub_iter;
        DBusMessageIter variant_iter;
        DBusMessageIter filter_list_iter;

        // filters
        dbus_message_iter_open_container(&sub_iter, DBUS_TYPE_DICT_ENTRY, nullptr, &sub_sub_iter);
        dbus_message_iter_append_basic(&sub_sub_iter, DBUS_TYPE_STRING, &STR_FILTERS);
        dbus_message_iter_open_container(
            &sub_sub_iter, DBUS_TYPE_VARIANT, "a(sa(us))", &variant_iter);
        dbus_message_iter_open_container(
            &variant_iter, DBUS_TYPE_ARRAY, "(sa(us))", &filter_list_iter);

        const char* curFilter = winFilter;
        const char* ptr = winFilter;
        // count
        for (unsigned long i = 1; *ptr; ++i)
        {
            if (i == filterIndex)
                curFilter = ptr;
            // name
            const char* name = ptr;
            ptr += strlen(ptr) + 1;
            // filter
            // malformed filter behaves similar to *.* (observed from experiments)
            if (!*ptr) {
                AppendWildcardFilter(filter_list_iter, name);
                break;
            }
            AppendSingleFilter(filter_list_iter, name, ptr);
            ptr += strlen(ptr) + 1;
        }

        dbus_message_iter_close_container(&variant_iter, &filter_list_iter);
        dbus_message_iter_close_container(&sub_sub_iter, &variant_iter);
        dbus_message_iter_close_container(&sub_iter, &sub_sub_iter);

        // current_filter
        dbus_message_iter_open_container(&sub_iter, DBUS_TYPE_DICT_ENTRY, nullptr, &sub_sub_iter);
        dbus_message_iter_append_basic(&sub_sub_iter, DBUS_TYPE_STRING, &STR_CURRENT_FILTER);
        dbus_message_iter_open_container(
            &sub_sub_iter, DBUS_TYPE_VARIANT, "(sa(us))", &variant_iter);
        const char* curFilterPattern = curFilter + strlen(curFilter) + 1;
        if (!*curFilterPattern)
            AppendWildcardFilter(variant_iter, curFilter);
        else
            AppendSingleFilter(variant_iter, curFilter, curFilterPattern);
        dbus_message_iter_close_container(&sub_sub_iter, &variant_iter);
        dbus_message_iter_close_container(&sub_iter, &sub_sub_iter);
    }
}

template <bool FilterEnabled>
void AppendOpenFileQueryDictEntryFilters(DBusMessageIter&,
                                         const nfdnfilteritem_t*,
                                         nfdfiltersize_t);
template <>
void AppendOpenFileQueryDictEntryFilters<true>(DBusMessageIter& sub_iter,
                                               const nfdnfilteritem_t* filterList,
                                               nfdfiltersize_t filterCount) {
    if (filterCount != 0) {
        DBusMessageIter sub_sub_iter;
        DBusMessageIter variant_iter;
        DBusMessageIter filter_list_iter;

        // filters
        dbus_message_iter_open_container(&sub_iter, DBUS_TYPE_DICT_ENTRY, nullptr, &sub_sub_iter);
        dbus_message_iter_append_basic(&sub_sub_iter, DBUS_TYPE_STRING, &STR_FILTERS);
        dbus_message_iter_open_container(
            &sub_sub_iter, DBUS_TYPE_VARIANT, "a(sa(us))", &variant_iter);
        dbus_message_iter_open_container(
            &variant_iter, DBUS_TYPE_ARRAY, "(sa(us))", &filter_list_iter);
        for (nfdfiltersize_t i = 0; i != filterCount; ++i) {
            AppendSingleFilter(filter_list_iter, filterList[i]);
        }
        AppendWildcardFilter(filter_list_iter);
        dbus_message_iter_close_container(&variant_iter, &filter_list_iter);
        dbus_message_iter_close_container(&sub_sub_iter, &variant_iter);
        dbus_message_iter_close_container(&sub_iter, &sub_sub_iter);

        // current_filter
        dbus_message_iter_open_container(&sub_iter, DBUS_TYPE_DICT_ENTRY, nullptr, &sub_sub_iter);
        dbus_message_iter_append_basic(&sub_sub_iter, DBUS_TYPE_STRING, &STR_CURRENT_FILTER);
        dbus_message_iter_open_container(
            &sub_sub_iter, DBUS_TYPE_VARIANT, "(sa(us))", &variant_iter);
        AppendSingleFilter(variant_iter, filterList[0]);
        dbus_message_iter_close_container(&sub_sub_iter, &variant_iter);
        dbus_message_iter_close_container(&sub_iter, &sub_sub_iter);
    }
}
template <>
void AppendOpenFileQueryDictEntryFilters<false>(DBusMessageIter&,
                                                const nfdnfilteritem_t*,
                                                nfdfiltersize_t) {}

void AppendSaveFileQueryDictEntryFilters(DBusMessageIter& sub_iter,
                                         const nfdnfilteritem_t* filterList,
                                         nfdfiltersize_t filterCount,
                                         const nfdnchar_t* defaultName) {
    if (filterCount != 0) {
        DBusMessageIter sub_sub_iter;
        DBusMessageIter variant_iter;
        DBusMessageIter filter_list_iter;

        // The extension of the defaultName (without the '.').  If NULL, it means that there is no
        // extension.
        const nfdnchar_t* extn = NULL;
        if (defaultName) {
            const nfdnchar_t* p = defaultName;
            while (*p) ++p;
            while (*--p != '.')
                ;
            ++p;
            if (*p) extn = p;
        }
        bool extn_matched = false;
        size_t selected_filter_index;

        // filters
        dbus_message_iter_open_container(&sub_iter, DBUS_TYPE_DICT_ENTRY, nullptr, &sub_sub_iter);
        dbus_message_iter_append_basic(&sub_sub_iter, DBUS_TYPE_STRING, &STR_FILTERS);
        dbus_message_iter_open_container(
            &sub_sub_iter, DBUS_TYPE_VARIANT, "a(sa(us))", &variant_iter);
        dbus_message_iter_open_container(
            &variant_iter, DBUS_TYPE_ARRAY, "(sa(us))", &filter_list_iter);
        for (nfdfiltersize_t i = 0; i != filterCount; ++i) {
            if (!extn_matched && extn) {
                extn_matched = AppendSingleFilterCheckExtn(filter_list_iter, filterList[i], extn);
                if (extn_matched) selected_filter_index = i;
            } else {
                AppendSingleFilter(filter_list_iter, filterList[i]);
            }
        }
        AppendWildcardFilter(filter_list_iter);
        dbus_message_iter_close_container(&variant_iter, &filter_list_iter);
        dbus_message_iter_close_container(&sub_sub_iter, &variant_iter);
        dbus_message_iter_close_container(&sub_iter, &sub_sub_iter);

        // current_filter
        dbus_message_iter_open_container(&sub_iter, DBUS_TYPE_DICT_ENTRY, nullptr, &sub_sub_iter);
        dbus_message_iter_append_basic(&sub_sub_iter, DBUS_TYPE_STRING, &STR_CURRENT_FILTER);
        dbus_message_iter_open_container(
            &sub_sub_iter, DBUS_TYPE_VARIANT, "(sa(us))", &variant_iter);
        if (extn_matched) {
            AppendSingleFilter(variant_iter, filterList[selected_filter_index]);
        } else {
            AppendWildcardFilter(variant_iter);
        }
        dbus_message_iter_close_container(&sub_sub_iter, &variant_iter);
        dbus_message_iter_close_container(&sub_iter, &sub_sub_iter);
    }
}

void AppendSaveFileQueryDictEntryCurrentName(DBusMessageIter& sub_iter, const char* name) {
    if (!name) return;
    DBusMessageIter sub_sub_iter;
    DBusMessageIter variant_iter;
    dbus_message_iter_open_container(&sub_iter, DBUS_TYPE_DICT_ENTRY, nullptr, &sub_sub_iter);
    dbus_message_iter_append_basic(&sub_sub_iter, DBUS_TYPE_STRING, &STR_CURRENT_NAME);
    dbus_message_iter_open_container(&sub_sub_iter, DBUS_TYPE_VARIANT, "s", &variant_iter);
    dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_STRING, &name);
    dbus_message_iter_close_container(&sub_sub_iter, &variant_iter);
    dbus_message_iter_close_container(&sub_iter, &sub_sub_iter);
}

void AppendSaveFileQueryDictEntryCurrentFolder(DBusMessageIter& sub_iter, const char* path) {
    if (!path) return;
    DBusMessageIter sub_sub_iter;
    DBusMessageIter variant_iter;
    DBusMessageIter array_iter;
    dbus_message_iter_open_container(&sub_iter, DBUS_TYPE_DICT_ENTRY, nullptr, &sub_sub_iter);
    dbus_message_iter_append_basic(&sub_sub_iter, DBUS_TYPE_STRING, &STR_CURRENT_FOLDER);
    dbus_message_iter_open_container(&sub_sub_iter, DBUS_TYPE_VARIANT, "ay", &variant_iter);
    dbus_message_iter_open_container(&variant_iter, DBUS_TYPE_ARRAY, "y", &array_iter);
    // Append string as byte array, including the terminating null byte as required by the portal.
    const char* p = path;
    do {
        dbus_message_iter_append_basic(&array_iter, DBUS_TYPE_BYTE, p);
    } while (*p++);
    dbus_message_iter_close_container(&variant_iter, &array_iter);
    dbus_message_iter_close_container(&sub_sub_iter, &variant_iter);
    dbus_message_iter_close_container(&sub_iter, &sub_sub_iter);
}

void AppendSaveFileQueryDictEntryCurrentFile(DBusMessageIter& sub_iter,
                                             const char* path,
                                             const char* name) {
    if (!path || !name) return;
    const size_t path_len = strlen(path);
    const size_t name_len = strlen(name);
    char* pathname;
    char* pathname_end;
    size_t pathname_len;
    if (path_len && path[path_len - 1] == '/') {
        pathname_len = path_len + name_len;
        pathname = NFDi_Malloc<char>(pathname_len + 1);
        pathname_end = pathname;
        pathname_end = copy(path, path + path_len, pathname_end);
        pathname_end = copy(name, name + name_len, pathname_end);
        *pathname_end++ = '\0';
    } else {
        pathname_len = path_len + 1 + name_len;
        pathname = NFDi_Malloc<char>(pathname_len + 1);
        pathname_end = pathname;
        pathname_end = copy(path, path + path_len, pathname_end);
        *pathname_end++ = '/';
        pathname_end = copy(name, name + name_len, pathname_end);
        *pathname_end++ = '\0';
    }
    Free_Guard<char> guard(pathname);
    if (access(pathname, F_OK) != 0) return;
    DBusMessageIter sub_sub_iter;
    DBusMessageIter variant_iter;
    DBusMessageIter array_iter;
    dbus_message_iter_open_container(&sub_iter, DBUS_TYPE_DICT_ENTRY, nullptr, &sub_sub_iter);
    dbus_message_iter_append_basic(&sub_sub_iter, DBUS_TYPE_STRING, &STR_CURRENT_FILE);
    dbus_message_iter_open_container(&sub_sub_iter, DBUS_TYPE_VARIANT, "ay", &variant_iter);
    dbus_message_iter_open_container(&variant_iter, DBUS_TYPE_ARRAY, "y", &array_iter);
    // This includes the terminating null character, which is required by the portal.
    for (const char* p = pathname; p != pathname_end; ++p) {
        dbus_message_iter_append_basic(&array_iter, DBUS_TYPE_BYTE, p);
    }
    dbus_message_iter_close_container(&variant_iter, &array_iter);
    dbus_message_iter_close_container(&sub_sub_iter, &variant_iter);
    dbus_message_iter_close_container(&sub_iter, &sub_sub_iter);
}

// Append OpenFile() portal params to the given query.
template <bool Multiple, bool Directory>
void AppendOpenFileQueryParams(DBusMessage* query,
                               const char* handle_token,
                               const nfdnfilteritem_t* filterList,
                               nfdfiltersize_t filterCount) {
    DBusMessageIter iter;
    dbus_message_iter_init_append(query, &iter);

    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &STR_EMPTY);

    AppendOpenFileQueryTitle<Multiple, Directory>(iter);

    DBusMessageIter sub_iter;
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &sub_iter);
    AppendOpenFileQueryDictEntryHandleToken(sub_iter, handle_token);
    AppendOpenFileQueryDictEntryMultiple<Multiple>(sub_iter);
    AppendOpenFileQueryDictEntryDirectory<Directory>(sub_iter);
    AppendOpenFileQueryDictEntryFilters<!Directory>(sub_iter, filterList, filterCount);
    dbus_message_iter_close_container(&iter, &sub_iter);
}

void AppendFileQueryParentWindow(DBusMessageIter& iter,
                                 decltype(NfdDialogParams::parentWindow) parentWindow) {
    if (parentWindow) {
        char* parentWindowStr = static_cast<char*>(alloca(5 + sizeof(parentWindow)));
        sprintf(parentWindowStr, "x11:%08lx", parentWindow);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &parentWindowStr);
    }
    else
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &STR_EMPTY);
}

// Append OpenFile() portal params to the given query.
template <bool Multiple, bool Directory>
void AppendOpenFileQueryParams(DBusMessage* query,
                               const char* handle_token,
                               NfdDialogParams* params)
{
    DBusMessageIter iter;
    dbus_message_iter_init_append(query, &iter);

    AppendFileQueryParentWindow(iter, params->parentWindow);

    AppendOpenFileQueryTitle<Multiple, Directory>(iter, params->title);

    DBusMessageIter sub_iter;
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &sub_iter);
    AppendOpenFileQueryDictEntryHandleToken(sub_iter, handle_token);
    if constexpr (Multiple)
        AppendOpenFileQueryDictEntryMultiple<true>(sub_iter);
    if constexpr (Directory)
        AppendOpenFileQueryDictEntryDirectory<true>(sub_iter);
    else
        AppendFileQueryDictEntryFilters(sub_iter, params->winFilter, params->filterIndex);
    dbus_message_iter_close_container(&iter, &sub_iter);
}

void AppendSaveFileQueryParams(DBusMessage* query,
                               const char* handle_token,
                               NfdDialogParams* params)
{
    DBusMessageIter iter;
    dbus_message_iter_init_append(query, &iter);

    AppendFileQueryParentWindow(iter, params->parentWindow);

    AppendSaveFileQueryTitle(iter);

    DBusMessageIter sub_iter;
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &sub_iter);
    AppendOpenFileQueryDictEntryHandleToken(sub_iter, handle_token);
    AppendFileQueryDictEntryFilters(sub_iter, params->winFilter, params->filterIndex);
    AppendSaveFileQueryDictEntryCurrentName(sub_iter, params->defaultName);
    AppendSaveFileQueryDictEntryCurrentFolder(sub_iter, params->defaultPath);
    AppendSaveFileQueryDictEntryCurrentFile(sub_iter, params->defaultPath, params->defaultName);
    dbus_message_iter_close_container(&iter, &sub_iter);
}

// Append SaveFile() portal params to the given query.
void AppendSaveFileQueryParams(DBusMessage* query,
                               const char* handle_token,
                               const nfdnfilteritem_t* filterList,
                               nfdfiltersize_t filterCount,
                               const nfdnchar_t* defaultPath,
                               const nfdnchar_t* defaultName) {
    DBusMessageIter iter;
    dbus_message_iter_init_append(query, &iter);

    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &STR_EMPTY);

    AppendSaveFileQueryTitle(iter);

    DBusMessageIter sub_iter;
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &sub_iter);
    AppendOpenFileQueryDictEntryHandleToken(sub_iter, handle_token);
    AppendSaveFileQueryDictEntryFilters(sub_iter, filterList, filterCount, defaultName);
    AppendSaveFileQueryDictEntryCurrentName(sub_iter, defaultName);
    AppendSaveFileQueryDictEntryCurrentFolder(sub_iter, defaultPath);
    AppendSaveFileQueryDictEntryCurrentFile(sub_iter, defaultPath, defaultName);
    dbus_message_iter_close_container(&iter, &sub_iter);
}

nfdresult_t ReadDictImpl(const char*, DBusMessageIter&) {
    return NFD_OKAY;
}

template <typename Callback, typename... Args>
nfdresult_t ReadDictImpl(const char* key,
                         DBusMessageIter& iter,
                         const char*& candidate_key,
                         Callback& candidate_callback,
                         Args&... args) {
    if (strcmp(key, candidate_key) == 0) {
        // this is the correct callback
        return candidate_callback(iter);
    } else {
        return ReadDictImpl(key, iter, args...);
    }
}

// Read a dictionary from the given iterator.  The type of the element under this iterator will be
// checked. The args are alternately key and callback. Key is a const char*, and callback is a
// function that returns nfdresult_t.  Return NFD_ERROR to stop processing and return immediately.
template <typename... Args>
nfdresult_t ReadDict(DBusMessageIter iter, Args... args) {
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
        NFDi_SetError("D-Bus response signal argument is not an array.");
        return NFD_ERROR;
    }
    DBusMessageIter sub_iter;
    dbus_message_iter_recurse(&iter, &sub_iter);
    while (dbus_message_iter_get_arg_type(&sub_iter) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter de_iter;
        dbus_message_iter_recurse(&sub_iter, &de_iter);
        if (dbus_message_iter_get_arg_type(&de_iter) != DBUS_TYPE_STRING) {
            NFDi_SetError("D-Bus response signal dict entry does not start with a string.");
            return NFD_ERROR;
        }
        const char* key;
        dbus_message_iter_get_basic(&de_iter, &key);
        if (!dbus_message_iter_next(&de_iter)) {
            NFDi_SetError("D-Bus response signal dict entry is missing one or more arguments.");
            return NFD_ERROR;
        }
        // unwrap the variant
        if (dbus_message_iter_get_arg_type(&de_iter) != DBUS_TYPE_VARIANT) {
            NFDi_SetError("D-Bus response signal dict entry value is not a variant.");
            return NFD_ERROR;
        }
        DBusMessageIter de_variant_iter;
        dbus_message_iter_recurse(&de_iter, &de_variant_iter);
        if (ReadDictImpl(key, de_variant_iter, args...) == NFD_ERROR) return NFD_ERROR;
        if (!dbus_message_iter_next(&sub_iter)) break;
    }
    return NFD_OKAY;
}

// Read the message, returning an iterator to the `results` dictionary of the response.  If response
// was okay, then returns NFD_OKAY and set `resultsIter` to the results dictionary iterator (this is
// the iterator to the entire dictionary (which has type DBUS_TYPE_ARRAY), not an iterator to the
// first item in the dictionary).  It does not check that this iterator is DBUS_TYPE_ARRAY; you
// should use ReadDict() which will check it.  Otherwise, returns NFD_CANCEL or NFD_ERROR as
// appropriate, and does not modify `resultsIter`. `resultsIter` can be copied by value.
nfdresult_t ReadResponseResults(DBusMessage* msg, DBusMessageIter& resultsIter) {
    DBusMessageIter iter;
    if (!dbus_message_iter_init(msg, &iter)) {
        NFDi_SetError("D-Bus response signal is missing one or more arguments.");
        return NFD_ERROR;
    }
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_UINT32) {
        NFDi_SetError("D-Bus response signal argument is not a uint32.");
        return NFD_ERROR;
    }
    dbus_uint32_t resp_code;
    dbus_message_iter_get_basic(&iter, &resp_code);
    if (resp_code != 0) {
        if (resp_code == 1) {
            // User pressed cancel
            return NFD_CANCEL;
        } else {
            // Some error occurred
            NFDi_SetError("D-Bus file dialog interaction was ended abruptly.");
            return NFD_ERROR;
        }
    }
    // User successfully responded
    if (!dbus_message_iter_next(&iter)) {
        NFDi_SetError("D-Bus response signal is missing one or more arguments.");
        return NFD_ERROR;
    }
    resultsIter = iter;
    return NFD_OKAY;
}

// Read the message.  If response was okay, then returns NFD_OKAY and set `uriIter` to the URI array
// iterator. Otherwise, returns NFD_CANCEL or NFD_ERROR as appropriate, and does not modify
// `uriIter`. `uriIter` can be copied by value.
nfdresult_t ReadResponseUris(DBusMessage* msg, DBusMessageIter& uriIter) {
    DBusMessageIter iter;
    const nfdresult_t res = ReadResponseResults(msg, iter);
    if (res != NFD_OKAY) return res;
    bool has_uris = false;
    if (ReadDict(iter, "uris", [&uriIter, &has_uris](DBusMessageIter& uris_iter) {
      if (dbus_message_iter_get_arg_type(&uris_iter) != DBUS_TYPE_ARRAY) {
          NFDi_SetError("D-Bus response signal URI iter is not an array.");
          return NFD_ERROR;
      }
      dbus_message_iter_recurse(&uris_iter, &uriIter);
      has_uris = true;
      return NFD_OKAY;
    }) == NFD_ERROR)
        return NFD_ERROR;

    if (!has_uris) {
        NFDi_SetError("D-Bus response signal has no URI field.");
        return NFD_ERROR;
    }
    return NFD_OKAY;
}

// Same as ReadResponseUris, but does not perform any message type checks.
// You should only use this if you previously used ReadResponseUris and it returned NFD_OKAY!
void ReadResponseUrisUnchecked(DBusMessage* msg, DBusMessageIter& uriIter) {
    DBusMessageIter iter;
    dbus_message_iter_init(msg, &iter);
    dbus_message_iter_next(&iter);
    ReadDict(iter, "uris", [&uriIter](DBusMessageIter& uris_iter) {
      dbus_message_iter_recurse(&uris_iter, &uriIter);
      return NFD_OKAY;
    });
}
nfdpathsetsize_t ReadResponseUrisUncheckedGetArraySize(DBusMessage* msg) {
    DBusMessageIter iter;
    dbus_message_iter_init(msg, &iter);
    dbus_message_iter_next(&iter);
    nfdpathsetsize_t sz = 0;  // Initialization will never be used, but we initialize it to prevent
    // the uninitialized warning otherwise.
    ReadDict(iter, "uris", [&sz](DBusMessageIter& uris_iter) {
      sz = dbus_message_iter_get_element_count(&uris_iter);
      return NFD_OKAY;
    });
    return sz;
}

// Read the response URI.  If response was okay, then returns NFD_OKAY and set file to it (the
// pointer is set to some string owned by msg, so you should not manually free it). Otherwise,
// returns NFD_CANCEL or NFD_ERROR as appropriate, and does not modify `file`.
nfdresult_t ReadResponseUrisSingle(DBusMessage* msg, const char*& file) {
    DBusMessageIter uri_iter;
    const nfdresult_t res = ReadResponseUris(msg, uri_iter);
    if (res != NFD_OKAY) return res;  // can be NFD_CANCEL or NFD_ERROR
    if (dbus_message_iter_get_arg_type(&uri_iter) != DBUS_TYPE_STRING) {
        NFDi_SetError("D-Bus response signal URI sub iter is not a string.");
        return NFD_ERROR;
    }
    dbus_message_iter_get_basic(&uri_iter, &file);
    return NFD_OKAY;
}

#ifdef NFD_APPEND_EXTENSION
// Read the response URI and selected extension (in the form "*.abc" or "*") (if any).  If response
// was okay, then returns NFD_OKAY and set file and extn to them (the pointer is set to some string
// owned by msg, so you should not manually free it).  `file` is the user-entered file name, and
// `extn` is the selected file extension (the first one if there are multiple extensions in the
// selected option) (this is NULL if "All files" is selected).  Otherwise, returns NFD_CANCEL or
// NFD_ERROR as appropriate, and does not modify `file` and `extn`.
nfdresult_t ReadResponseUrisSingleAndCurrentExtension(DBusMessage* msg,
                                                      const char*& file,
                                                      const char*& extn) {
    DBusMessageIter iter;
    const nfdresult_t res = ReadResponseResults(msg, iter);
    if (res != NFD_OKAY) return res;
    const char* tmp_file = nullptr;
    const char* tmp_extn = nullptr;
    if (ReadDict(
            iter,
            "uris",
            [&tmp_file](DBusMessageIter& uris_iter) {
                if (dbus_message_iter_get_arg_type(&uris_iter) != DBUS_TYPE_ARRAY) {
                    NFDi_SetError("D-Bus response signal URI iter is not an array.");
                    return NFD_ERROR;
                }
                DBusMessageIter uri_iter;
                dbus_message_iter_recurse(&uris_iter, &uri_iter);
                if (dbus_message_iter_get_arg_type(&uri_iter) != DBUS_TYPE_STRING) {
                    NFDi_SetError("D-Bus response signal URI sub iter is not a string.");
                    return NFD_ERROR;
                }
                dbus_message_iter_get_basic(&uri_iter, &tmp_file);
                return NFD_OKAY;
            },
            "current_filter",
            [&tmp_extn](DBusMessageIter& current_filter_iter) {
                // current_filter is best_effort, so if we fail, we still return NFD_OKAY.
                if (dbus_message_iter_get_arg_type(&current_filter_iter) != DBUS_TYPE_STRUCT) {
                    // NFDi_SetError("D-Bus response signal current_filter iter is not a struct.");
                    return NFD_OKAY;
                }
                DBusMessageIter current_filter_struct_iter;
                dbus_message_iter_recurse(&current_filter_iter, &current_filter_struct_iter);
                if (!dbus_message_iter_next(&current_filter_struct_iter)) {
                    // NFDi_SetError("D-Bus response signal current_filter struct iter ended
                    // prematurely.");
                    return NFD_OKAY;
                }
                if (dbus_message_iter_get_arg_type(&current_filter_struct_iter) !=
                    DBUS_TYPE_ARRAY) {
                    // NFDi_SetError("D-Bus response signal URI sub iter is not a string.");
                    return NFD_OKAY;
                }
                DBusMessageIter current_filter_array_iter;
                dbus_message_iter_recurse(&current_filter_struct_iter, &current_filter_array_iter);
                if (dbus_message_iter_get_arg_type(&current_filter_array_iter) !=
                    DBUS_TYPE_STRUCT) {
                    // NFDi_SetError("D-Bus response signal current_filter iter is not a struct.");
                    return NFD_OKAY;
                }
                DBusMessageIter current_filter_extn_iter;
                dbus_message_iter_recurse(&current_filter_array_iter, &current_filter_extn_iter);
                if (dbus_message_iter_get_arg_type(&current_filter_extn_iter) != DBUS_TYPE_UINT32) {
                    // NFDi_SetError("D-Bus response signal URI sub iter is not a string.");
                    return NFD_OKAY;
                }
                dbus_uint32_t type;
                dbus_message_iter_get_basic(&current_filter_extn_iter, &type);
                if (type != 0) {
                    // NFDi_SetError("Wrong filter type.");
                    return NFD_OKAY;
                }
                if (!dbus_message_iter_next(&current_filter_extn_iter)) {
                    // NFDi_SetError("D-Bus response signal current_filter struct iter ended
                    // prematurely.");
                    return NFD_OKAY;
                }
                if (dbus_message_iter_get_arg_type(&current_filter_extn_iter) != DBUS_TYPE_STRING) {
                    // NFDi_SetError("D-Bus response signal URI sub iter is not a string.");
                    return NFD_OKAY;
                }
                dbus_message_iter_get_basic(&current_filter_extn_iter, &tmp_extn);
                return NFD_OKAY;
            }) == NFD_ERROR)
        return NFD_ERROR;

    if (!tmp_file) {
        NFDi_SetError("D-Bus response signal has no URI field.");
        return NFD_ERROR;
    }
    file = tmp_file;
    extn = tmp_extn;
    return NFD_OKAY;
}
#endif

// Appends up to 64 random chars to the given pointer.  Returns the end of the appended chars.
char* Generate64RandomChars(char* out) {
    size_t amount = 32;
    while (amount > 0) {
        unsigned char buf[32];
        ssize_t res = getrandom(buf, amount, 0);
        if (res == -1) {
            if (errno == EINTR)
                continue;
            else
                break;  // too bad, urandom isn't working well
        }
        amount -= res;
        // we encode each random char using two chars, since they must be [A-Z][a-z][0-9]_
        for (size_t i = 0; i != static_cast<size_t>(res); ++i) {
            *out++ = 'A' + static_cast<char>(buf[i] & 15);
            *out++ = 'A' + static_cast<char>(buf[i] >> 4);
        }
    }
    return out;
}

constexpr const char STR_RESPONSE_HANDLE_PREFIX[] = "/org/freedesktop/portal/desktop/request/";
constexpr size_t STR_RESPONSE_HANDLE_PREFIX_LEN =
    sizeof(STR_RESPONSE_HANDLE_PREFIX) - 1;  // -1 to remove the \0.

// Allocates and returns a path like "/org/freedesktop/portal/desktop/request/SENDER/TOKEN" with
// randomly generated TOKEN as recommended by flatpak.  `handle_token_ptr` is a pointer to the
// TOKEN part.
char* MakeUniqueObjectPath(const char** handle_token_ptr) {
    const char* sender = dbus_unique_name;
    if (*sender == ':') ++sender;
    const size_t sender_len = strlen(sender);
    const size_t sz = STR_RESPONSE_HANDLE_PREFIX_LEN + sender_len + 1 +
                      64;  // 1 for '/', followed by 64 random chars
    char* path = NFDi_Malloc<char>(sz + 1);
    char* path_ptr = path;
    path_ptr = copy(STR_RESPONSE_HANDLE_PREFIX,
                    STR_RESPONSE_HANDLE_PREFIX + STR_RESPONSE_HANDLE_PREFIX_LEN,
                    path_ptr);
    path_ptr = transform(
        sender, sender + sender_len, path_ptr, [](char ch) { return ch != '.' ? ch : '_'; });
    *path_ptr++ = '/';
    *handle_token_ptr = path_ptr;
    path_ptr = Generate64RandomChars(path_ptr);
    *path_ptr = '\0';
    return path;
}

constexpr const char STR_RESPONSE_SUBSCRIPTION_PATH_1[] =
    "type='signal',sender='org.freedesktop.portal.Desktop',path='";
constexpr const char STR_RESPONSE_SUBSCRIPTION_PATH_1_LEN =
    sizeof(STR_RESPONSE_SUBSCRIPTION_PATH_1) - 1;
constexpr const char STR_RESPONSE_SUBSCRIPTION_PATH_2[] =
    "',interface='org.freedesktop.portal.Request',member='Response',destination='";
constexpr const char STR_RESPONSE_SUBSCRIPTION_PATH_2_LEN =
    sizeof(STR_RESPONSE_SUBSCRIPTION_PATH_2) - 1;
constexpr const char STR_RESPONSE_SUBSCRIPTION_PATH_3[] = "'";
constexpr const char STR_RESPONSE_SUBSCRIPTION_PATH_3_LEN =
    sizeof(STR_RESPONSE_SUBSCRIPTION_PATH_3) - 1;

class DBusSignalSubscriptionHandler {
   private:
    char* sub_cmd;

   public:
    DBusSignalSubscriptionHandler() : sub_cmd(nullptr) {}
    ~DBusSignalSubscriptionHandler() {
        if (sub_cmd) Unsubscribe();
    }

    nfdresult_t Subscribe(const char* handle_path) {
        if (sub_cmd) Unsubscribe();
        sub_cmd = MakeResponseSubscriptionPath(handle_path, dbus_unique_name);
        DBusError err;
        dbus_error_init(&err);
        dbus_bus_add_match(dbus_conn, sub_cmd, &err);
        if (dbus_error_is_set(&err)) {
            dbus_error_free(&dbus_err);
            dbus_move_error(&err, &dbus_err);
            NFDi_SetError(dbus_err.message);
            return NFD_ERROR;
        }
        return NFD_OKAY;
    }

    void Unsubscribe() {
        DBusError err;
        dbus_error_init(&err);
        dbus_bus_remove_match(dbus_conn, sub_cmd, &err);
        NFDi_Free(sub_cmd);
        sub_cmd = nullptr;
        dbus_error_free(
            &err);  // silence unsubscribe errors, because this is intuitively part of 'cleanup'
    }

   private:
    static char* MakeResponseSubscriptionPath(const char* handle_path, const char* unique_name) {
        const size_t handle_path_len = strlen(handle_path);
        const size_t unique_name_len = strlen(unique_name);
        const size_t sz = STR_RESPONSE_SUBSCRIPTION_PATH_1_LEN + handle_path_len +
                          STR_RESPONSE_SUBSCRIPTION_PATH_2_LEN + unique_name_len +
                          STR_RESPONSE_SUBSCRIPTION_PATH_3_LEN;
        char* res = NFDi_Malloc<char>(sz + 1);
        char* res_ptr = res;
        res_ptr = copy(STR_RESPONSE_SUBSCRIPTION_PATH_1,
                       STR_RESPONSE_SUBSCRIPTION_PATH_1 + STR_RESPONSE_SUBSCRIPTION_PATH_1_LEN,
                       res_ptr);
        res_ptr = copy(handle_path, handle_path + handle_path_len, res_ptr);
        res_ptr = copy(STR_RESPONSE_SUBSCRIPTION_PATH_2,
                       STR_RESPONSE_SUBSCRIPTION_PATH_2 + STR_RESPONSE_SUBSCRIPTION_PATH_2_LEN,
                       res_ptr);
        res_ptr = copy(unique_name, unique_name + unique_name_len, res_ptr);
        res_ptr = copy(STR_RESPONSE_SUBSCRIPTION_PATH_3,
                       STR_RESPONSE_SUBSCRIPTION_PATH_3 + STR_RESPONSE_SUBSCRIPTION_PATH_3_LEN,
                       res_ptr);
        *res_ptr = '\0';
        return res;
    }
};

// Returns true if ch is in [0-9A-Za-z], false otherwise.
bool IsHex(char ch) {
    return ('0' <= ch && ch <= '9') || ('A' <= ch && ch <= 'F') || ('a' <= ch && ch <= 'f');
}

// Returns the hexadecimal value contained in the char.  Precondition: IsHex(ch)
char ParseHexUnchecked(char ch) {
    if ('0' <= ch && ch <= '9') return ch - '0';
    if ('A' <= ch && ch <= 'F') return ch - ('A' - 10);
    if ('a' <= ch && ch <= 'f') return ch - ('a' - 10);
#if defined(__GNUC__)
    __builtin_unreachable();
#endif
}

// Returns true if the given file URI is decodable (i.e. not malformed), and false otherwise.
// If this function returns true, then `out` will be populated with the length of the decoded URI
// and `fileUriEnd` will point to the trailing null byte of `fileUri`. Otherwise, `out` and
// `fileUriEnd` will be unmodified.
bool TryUriDecodeLen(const char* fileUri, size_t& out, const char*& fileUriEnd) {
    size_t len = 0;
    while (*fileUri) {
        if (*fileUri != '%') {
            ++fileUri;
        } else {
            if (*(fileUri + 1) == '\0' || *(fileUri + 2) == '\0') {
                return false;
            }
            if (!IsHex(*(fileUri + 1)) || !IsHex(*(fileUri + 2))) {
                return false;
            }
            fileUri += 3;
        }
        ++len;
    }
    out = len;
    fileUriEnd = fileUri;
    return true;
}

// Decodes the given URI and writes it to `outPath`.  The caller must ensure that the given URI is
// not malformed (typically with a prior call to `TryUriDecodeLen`).  This function does not write
// any trailing null character.
char* UriDecodeUnchecked(const char* fileUri, const char* fileUriEnd, char* outPath) {
    while (fileUri != fileUriEnd) {
        if (*fileUri != '%') {
            *outPath++ = *fileUri++;
        } else {
            ++fileUri;
            const char high_nibble = ParseHexUnchecked(*fileUri++);
            const char low_nibble = ParseHexUnchecked(*fileUri++);
            *outPath++ = (high_nibble << 4) | low_nibble;
        }
    }
    return outPath;
}

constexpr const char FILE_URI_PREFIX[] = "file://";
constexpr size_t FILE_URI_PREFIX_LEN = sizeof(FILE_URI_PREFIX) - 1;

// If fileUri starts with "file://", strips that prefix and URI-decodes the remaining part to a new
// buffer, and make outPath point to it, and returns NFD_OKAY. Otherwise, does not modify outPath
// and returns NFD_ERROR (with the correct error set)
nfdresult_t AllocAndCopyFilePath(const char* fileUri, char*& outPath, size_t* outSize = nullptr) {
    const char* prefix_begin = FILE_URI_PREFIX;
    const char* const prefix_end = FILE_URI_PREFIX + FILE_URI_PREFIX_LEN;
    for (; prefix_begin != prefix_end; ++prefix_begin, ++fileUri) {
        if (*prefix_begin != *fileUri) {
            NFDi_SetError("D-Bus freedesktop portal returned a URI that is not a file URI.");
            return NFD_ERROR;
        }
    }
    size_t decoded_len;
    const char* file_uri_end;
    if (!TryUriDecodeLen(fileUri, decoded_len, file_uri_end)) {
        NFDi_SetError("D-Bus freedesktop portal returned a malformed URI.");
        return NFD_ERROR;
    }
    char* const path_without_prefix = NFDi_Malloc<char>(decoded_len + 1);
    char* const out_end = UriDecodeUnchecked(fileUri, file_uri_end, path_without_prefix);
    *out_end = '\0';
    outPath = path_without_prefix;
    if (outSize)
        *outSize = decoded_len + 1;
    return NFD_OKAY;
}

namespace policy
{
struct FilePathPolicyBase {};

struct Basename : FilePathPolicyBase {};

struct Dirname : FilePathPolicyBase {};

struct FullPath : FilePathPolicyBase {};
}

template <typename T>
concept FilePathPolicy = std::is_base_of_v<policy::FilePathPolicyBase, T>;

namespace details
{
template <typename T>
struct Dummy : std::false_type {
};
}

void expandOnDemand(char*& base, char*& ptr, int& size, size_t targetSize)
{
    int origSize = size;
    ptrdiff_t offset = ptr - base;
    // expand by 2 times if smaller
    while (size - offset < targetSize)
        size <<= 1;
    if (origSize != size) {
        base = NFDi_Realloc<char>(base, size);
        ptr = base + offset;
    }
}

template <FilePathPolicy Policy>
nfdresult_t copyFileInfo(const char* fileUri, char*& outPathList, char*& curOutPath, int& outPathSize) {
    const char* prefix_begin = FILE_URI_PREFIX;
    const char* const prefix_end = FILE_URI_PREFIX + FILE_URI_PREFIX_LEN;
    for (; prefix_begin != prefix_end; ++prefix_begin, ++fileUri) {
        if (*prefix_begin != *fileUri) {
            NFDi_SetError("D-Bus freedesktop portal returned a URI that is not a file URI.");
            return NFD_ERROR;
        }
    }
    size_t decoded_len;
    const char* file_uri_end;
    if (!TryUriDecodeLen(fileUri, decoded_len, file_uri_end)) {
        NFDi_SetError("D-Bus freedesktop portal returned a malformed URI.");
        return NFD_ERROR;
    }
    if constexpr (std::same_as<Policy, policy::FullPath>) {
        // expand by 2 times if smaller
        expandOnDemand(outPathList, curOutPath, outPathSize, decoded_len + 1);
        char* const out_end = UriDecodeUnchecked(fileUri, file_uri_end, curOutPath);
        *out_end = '\0';
        curOutPath = out_end + 1;
    }
    else if constexpr (std::same_as<Policy, policy::Basename> ||
                       std::same_as<Policy, policy::Dirname>) {
        char* (*segFunc)(char*);
        if constexpr (std::same_as<Policy, policy::Basename>)
            segFunc = basename;
        else
            segFunc = dirname;
        char* buf = static_cast<char*>(alloca(decoded_len + 1));
        char* const bufEnd = UriDecodeUnchecked(fileUri, file_uri_end, buf);
        *bufEnd = '\0';
        char* const segment = segFunc(buf);
        size_t segmentLen = strlen(segment);
        expandOnDemand(outPathList, curOutPath, outPathSize, segmentLen + 1);
        curOutPath = copy(segment, bufEnd + 1, curOutPath);
    }
    else {
        static_assert(details::Dummy<Policy>::value, "policy not implemented");
    }
    return NFD_OKAY;
}

template <FilePathPolicy Policy = policy::FullPath>
nfdresult_t NFD_PathSet_GetPathWin(const nfdpathset_t* pathSet,
                                   nfdpathsetsize_t index,
                                   char*& outPathList,
                                   char*& curOutPath,
                                   int& outPathSize)
{
    assert(pathSet);
    DBusMessage* msg = const_cast<DBusMessage*>(static_cast<const DBusMessage*>(pathSet));
    DBusMessageIter uri_iter;
    ReadResponseUrisUnchecked(msg, uri_iter);
    while (index > 0) {
        --index;
        if (!dbus_message_iter_next(&uri_iter)) {
            NFDi_SetError("Index out of bounds.");
            return NFD_ERROR;
        }
    }
    if (dbus_message_iter_get_arg_type(&uri_iter) != DBUS_TYPE_STRING) {
        NFDi_SetError("D-Bus response signal URI sub iter is not a string.");
        return NFD_ERROR;
    }
    const char* uri;
    dbus_message_iter_get_basic(&uri_iter, &uri);
    return copyFileInfo<Policy>(uri, outPathList, curOutPath, outPathSize);
}

//template <typename Derived, bool Multiple>
//class NfdFilePathProcessor;
//
//template <typename T>
//class NfdFilePathProcessor<T, true> {
//    [[nodiscard]] T& derived() { return *static_cast<T*>(this); }
//    [[nodiscard]] const T& derived() const { return *static_cast<const T*>(this); }
//};
//
//template <typename T>
//class NfdFilePathProcessor<T, false> {
//
//    [[nodiscard]] T& derived() { return *static_cast<T*>(this); }
//    [[nodiscard]] const T& derived() const { return *static_cast<const T*>(this); }
//
//    nfdresult_t allocAndCopyFilePath(const char* fileUri)
//    {
//        const char* prefix_begin = FILE_URI_PREFIX;
//        const char* const prefix_end = FILE_URI_PREFIX + FILE_URI_PREFIX_LEN;
//        for (; prefix_begin != prefix_end; ++prefix_begin, ++fileUri) {
//            if (*prefix_begin != *fileUri) {
//                NFDi_SetError("D-Bus freedesktop portal returned a URI that is not a file URI.");
//                return NFD_ERROR;
//            }
//        }
//        size_t decoded_len;
//        const char* file_uri_end;
//        if (!TryUriDecodeLen(fileUri, decoded_len, file_uri_end)) {
//            NFDi_SetError("D-Bus freedesktop portal returned a malformed URI.");
//            return NFD_ERROR;
//        }
//        char* const path_without_prefix = NFDi_Malloc<char>(decoded_len + 1);
//        char* const out_end = UriDecodeUnchecked(fileUri, file_uri_end, path_without_prefix);
//        *out_end = '\0';
//        {
//            ScopedLock lock(&mutex);
//            outPath = path_without_prefix;
//            outPathSize = decoded_len + 1;
//        }
//        return NFD_OKAY;
//    }
//protected:
//
//};

class NfdDialogMonitor {
    char* outPath{};
    size_t outPathSize{};
    nfdresult_t resultCode{};
    bool completed{};

    mutable pthread_mutex_t mutex{};
    pthread_t thread{};

    class ScopedLock {
        pthread_mutex_t* m_;
    public:
        explicit ScopedLock(pthread_mutex_t* m) noexcept : m_(m) { pthread_mutex_lock(m_); }
        ~ScopedLock() { pthread_mutex_unlock(m_); }
    };

    class MutexDestroyGuard {
        pthread_mutex_t* m_;
    public:
        explicit MutexDestroyGuard(pthread_mutex_t* m) noexcept : m_(m) { }
        ~MutexDestroyGuard()
        {
            pthread_mutex_destroy(m_);
        }
    };

    nfdresult_t allocAndCopyFilePath(const char* fileUri)
    {
        const char* prefix_begin = FILE_URI_PREFIX;
        const char* const prefix_end = FILE_URI_PREFIX + FILE_URI_PREFIX_LEN;
        for (; prefix_begin != prefix_end; ++prefix_begin, ++fileUri) {
            if (*prefix_begin != *fileUri) {
                NFDi_SetError("D-Bus freedesktop portal returned a URI that is not a file URI.");
                return NFD_ERROR;
            }
        }
        size_t decoded_len;
        const char* file_uri_end;
        if (!TryUriDecodeLen(fileUri, decoded_len, file_uri_end)) {
            NFDi_SetError("D-Bus freedesktop portal returned a malformed URI.");
            return NFD_ERROR;
        }
        char* const path_without_prefix = NFDi_Malloc<char>(decoded_len + 1);
        char* const out_end = UriDecodeUnchecked(fileUri, file_uri_end, path_without_prefix);
        *out_end = '\0';
        {
            ScopedLock lock(&mutex);
            outPath = path_without_prefix;
            outPathSize = decoded_len + 1;
        }
        return NFD_OKAY;
    }

    nfdresult_t copySingleFilepath(DBusMessage* msg)
    {
        const char* uri;
        {
            const nfdresult_t res = ReadResponseUrisSingle(msg, uri);
            if (res != NFD_OKAY) {
                return res;
            }
        }
        if (nfdresult_t res = allocAndCopyFilePath(uri); res != NFD_OKAY) {
            return res;
        }
        return NFD_OKAY;
    }

    nfdresult_t copyMultipleFilePath(DBusMessage* msg)
    {
        DBusMessageIter uri_iter;
        nfdresult_t res = ReadResponseUris(msg, uri_iter);
        if (res != NFD_OKAY)
            return res;

        nfdpathsetsize_t numPaths;
        NFD_PathSet_GetCount(msg, &numPaths);

        int pathListSize = 256;
        char* tmpOutPath = NFDi_Malloc<char>(pathListSize);
        char* curOutPath = tmpOutPath;

        if (numPaths == 1)
            res = NFD_PathSet_GetPathWin<policy::FullPath>(
                msg, 0, tmpOutPath, curOutPath, pathListSize);
        else
            res = NFD_PathSet_GetPathWin<policy::Dirname>(
                msg, 0, tmpOutPath, curOutPath, pathListSize);

        if (res != NFD_OKAY) {
            NFDi_Free(tmpOutPath);
            return res;
        }

        for (nfdpathsetsize_t i = 1; i < numPaths; ++i) {
            if (NFD_PathSet_GetPathWin<policy::Basename>(
                msg, i, tmpOutPath, curOutPath, pathListSize) != NFD_OKAY) {
                NFDi_Free(tmpOutPath);
                return NFD_ERROR;
            }
        }

        if (pathListSize == curOutPath - tmpOutPath) {
            ptrdiff_t offset = curOutPath - tmpOutPath;
            tmpOutPath = NFDi_Realloc<char>(tmpOutPath, ++pathListSize);
            curOutPath = tmpOutPath + offset;
        }
        *curOutPath = '\0';  // double null-terminate
        {
            ScopedLock lock(&mutex);
            outPath = tmpOutPath;
            outPathSize = curOutPath - tmpOutPath + 1;
        }

        return NFD_OKAY;
    }

    template <bool Multiple>
    static void* monitorUntilReturn(void* args)
    {
        NfdDialogMonitor* self = static_cast<NfdDialogMonitor*>(args);

        // Wait and read the response
        // const char* file = nullptr;
        do {
            while (true) {
                DBusMessage* msg = dbus_connection_pop_message(dbus_conn);
                if (!msg) break;

                DBusMessage_Guard guard(msg);

                if (dbus_message_is_signal(msg, "org.freedesktop.portal.Request", "Response")) {
                    nfdresult_t res;

                    if constexpr (Multiple)
                        res = self->copyMultipleFilePath(msg);
                    else
                        res = self->copySingleFilepath(msg);

                    ScopedLock lock(&self->mutex);
                    self->resultCode = res;
                    self->completed = true;
                    return nullptr;
                }
            }
        } while (dbus_connection_read_write(dbus_conn, -1));

        NFDi_SetError("D-Bus freedesktop portal did not give us a reply.");
        {
            ScopedLock lock(&self->mutex);
            self->resultCode = NFD_ERROR;
            self->completed = true;
        }

        return nullptr;
    }

    NfdDialogMonitor() noexcept = default;

public:
    NfdDialogMonitor(const NfdDialogMonitor&) = delete;

    NfdDialogMonitor& operator=(const NfdDialogMonitor&) = delete;

    ~NfdDialogMonitor()
    {
        NFDi_Free(outPath);
        pthread_mutex_destroy(&mutex);
    }

    template <bool Multiple>
    static NfdDialogMonitor* create() noexcept
    {
        auto* ret = NFDi_Malloc<NfdDialogMonitor>(sizeof(NfdDialogMonitor));
        new (ret) NfdDialogMonitor();
        if (pthread_mutex_init(&ret->mutex, nullptr)) {
            NFDi_SetError("pthread_mutex_init failed");
            NFDi_Free(ret);
            return nullptr;
        }
        MutexDestroyGuard mutexDestroyGuard{&ret->mutex};
        if (int err = pthread_create(&ret->thread, nullptr, monitorUntilReturn<Multiple>, ret)) {
            NFDi_SetError("pthread_create failed");
            NFDi_Free(ret);
            return nullptr;
        }
        return ret;
    }

    static void destroy(NfdDialogMonitor* monitor) noexcept
    {
        monitor->~NfdDialogMonitor();
        NFDi_Free(monitor);
    }

    [[nodiscard]]
    bool hasDialogReturned() const noexcept
    {
        ScopedLock lock(&mutex);
        return completed;
    }

    [[nodiscard]]
    nfdresult_t getDialogResult(NfdDialogResponse* result) noexcept
    {
        ScopedLock lock(&mutex);
        if (!completed) {
            NFDi_SetError("response not ready");
            return NFD_ERROR;
        }
        if (resultCode != NFD_OKAY)
            return resultCode;
        *result->outPath = outPath;
        outPath = nullptr;
        result->outPathSize = outPathSize;
        return NFD_OKAY;
    }
};


#ifdef NFD_APPEND_EXTENSION
bool TryGetValidExtension(const char* extn,
                          const char*& trimmed_extn,
                          const char*& trimmed_extn_end) {
    if (!extn) return false;
    if (*extn != '*') return false;
    ++extn;
    if (*extn != '.') return false;
    trimmed_extn = extn;
    for (++extn; *extn != '\0'; ++extn)
        ;
    ++extn;
    trimmed_extn_end = extn;
    return true;
}

// Like AllocAndCopyFilePath, but if `fileUri` has no extension and `extn` is usable, appends the
// extension. `extn` could be null, in which case no extension will ever be appended. `extn` is
// expected to be either in the form "*.abc" or "*", but this function will check for it, and ignore
// the extension if it is not in the correct form.
nfdresult_t AllocAndCopyFilePathWithExtn(const char* fileUri, const char* extn, char*& outPath) {
    const char* prefix_begin = FILE_URI_PREFIX;
    const char* const prefix_end = FILE_URI_PREFIX + FILE_URI_PREFIX_LEN;
    for (; prefix_begin != prefix_end; ++prefix_begin, ++fileUri) {
        if (*prefix_begin != *fileUri) {
            NFDi_SetError("D-Bus freedesktop portal returned a URI that is not a file URI.");
            return NFD_ERROR;
        }
    }

    size_t decoded_len;
    const char* file_uri_end;
    if (!TryUriDecodeLen(fileUri, decoded_len, file_uri_end)) {
        NFDi_SetError("D-Bus freedesktop portal returned a malformed URI.");
        return NFD_ERROR;
    }

    const char* file_it = file_uri_end;
    // The following loop condition is safe because `FILE_URI_PREFIX` ends with '/',
    // so we won't iterate past the beginning of the URI.
    // Also in UTF-8 all non-ASCII code points are encoded using bytes 128-255 so every '.' or '/'
    // is also '.' or '/' in UTF-8.
    do {
        --file_it;
    } while (*file_it != '/' && *file_it != '.');
    const char* trimmed_extn;      // includes the '.'
    const char* trimmed_extn_end;  // includes the '\0'
    if (*file_it == '.' || !TryGetValidExtension(extn, trimmed_extn, trimmed_extn_end)) {
        // has file extension already or no valid extension in `extn`
        char* const path_without_prefix = NFDi_Malloc<char>(decoded_len + 1);
        char* const out_end = UriDecodeUnchecked(fileUri, file_uri_end, path_without_prefix);
        *out_end = '\0';
        outPath = path_without_prefix;
    } else {
        // no file extension and we have a valid extension
        char* const path_without_prefix =
            NFDi_Malloc<char>(decoded_len + (trimmed_extn_end - trimmed_extn));
        char* const out_mid = UriDecodeUnchecked(fileUri, file_uri_end, path_without_prefix);
        char* const out_end = copy(trimmed_extn, trimmed_extn_end, out_mid);
        *out_end = '\0';
        outPath = path_without_prefix;
    }
    return NFD_OKAY;
}
#endif

// DBus wrapper function that helps invoke the portal for all OpenFile() variants.
// This function returns NFD_OKAY iff outMsg gets set (to the returned message).
// Caller is responsible for freeing the outMsg using dbus_message_unref() (or use
// DBusMessage_Guard).
template <bool Multiple, bool Directory>
nfdresult_t NFD_DBus_OpenFile(DBusMessage*& outMsg,
                              const nfdnfilteritem_t* filterList,
                              nfdfiltersize_t filterCount) {
    const char* handle_token_ptr;
    char* handle_obj_path = MakeUniqueObjectPath(&handle_token_ptr);
    Free_Guard<char> handle_obj_path_guard(handle_obj_path);

    DBusError err;  // need a separate error object because we don't want to mess with the old one
    // if it's stil set
    dbus_error_init(&err);

    // Subscribe to the signal using the handle_obj_path
    DBusSignalSubscriptionHandler signal_sub;
    nfdresult_t res = signal_sub.Subscribe(handle_obj_path);
    if (res != NFD_OKAY) return res;

    // TODO: use XOpenDisplay()/XGetInputFocus() to find xid of window... but what should one do on
    // Wayland?

    DBusMessage* query = dbus_message_new_method_call("org.freedesktop.portal.Desktop",
                                                      "/org/freedesktop/portal/desktop",
                                                      "org.freedesktop.portal.FileChooser",
                                                      "OpenFile");
    DBusMessage_Guard query_guard(query);
    AppendOpenFileQueryParams<Multiple, Directory>(
        query, handle_token_ptr, filterList, filterCount);

    DBusMessage* reply =
        dbus_connection_send_with_reply_and_block(dbus_conn, query, DBUS_TIMEOUT_INFINITE, &err);
    if (!reply) {
        dbus_error_free(&dbus_err);
        dbus_move_error(&err, &dbus_err);
        NFDi_SetError(dbus_err.message);
        return NFD_ERROR;
    }
    DBusMessage_Guard reply_guard(reply);

    // Check the reply and update our signal subscription if necessary
    {
        DBusMessageIter iter;
        if (!dbus_message_iter_init(reply, &iter)) {
            NFDi_SetError("D-Bus reply is missing an argument.");
            return NFD_ERROR;
        }
        if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_OBJECT_PATH) {
            NFDi_SetError("D-Bus reply is not an object path.");
            return NFD_ERROR;
        }

        const char* path;
        dbus_message_iter_get_basic(&iter, &path);
        if (strcmp(path, handle_obj_path) != 0) {
            // needs to change our signal subscription
            signal_sub.Subscribe(path);
        }
    }

    // Wait and read the response
    // const char* file = nullptr;
    do {
        while (true) {
            DBusMessage* msg = dbus_connection_pop_message(dbus_conn);
            if (!msg) break;

            if (dbus_message_is_signal(msg, "org.freedesktop.portal.Request", "Response")) {
                // this is the response we're looking for
                outMsg = msg;
                return NFD_OKAY;
            }

            dbus_message_unref(msg);
        }
    } while (dbus_connection_read_write(dbus_conn, -1));

    NFDi_SetError("D-Bus freedesktop portal did not give us a reply.");
    return NFD_ERROR;
}
template <bool Multiple, bool Directory>
nfdresult_t NFD_DBus_ShowOpenFileDialog(NfdDialogParams* params)
{
    const char* handle_token_ptr;
    char* handle_obj_path = MakeUniqueObjectPath(&handle_token_ptr);
    Free_Guard<char> handle_obj_path_guard(handle_obj_path);

    DBusError err;  // need a separate error object because we don't want to mess with the old one
    // if it's stil set
    dbus_error_init(&err);

    // Subscribe to the signal using the handle_obj_path
    DBusSignalSubscriptionHandler signal_sub;
    nfdresult_t res = signal_sub.Subscribe(handle_obj_path);
    if (res != NFD_OKAY) return res;

    // TODO: use XOpenDisplay()/XGetInputFocus() to find xid of window... but what should one do on
    // Wayland?

    DBusMessage* query = dbus_message_new_method_call("org.freedesktop.portal.Desktop",
                                                      "/org/freedesktop/portal/desktop",
                                                      "org.freedesktop.portal.FileChooser",
                                                      "OpenFile");
    DBusMessage_Guard query_guard(query);
    AppendOpenFileQueryParams<Multiple, Directory>(
        query, handle_token_ptr, params);

    DBusMessage* reply =
        dbus_connection_send_with_reply_and_block(dbus_conn, query, DBUS_TIMEOUT_INFINITE, &err);
    if (!reply) {
        dbus_error_free(&dbus_err);
        dbus_move_error(&err, &dbus_err);
        NFDi_SetError(dbus_err.message);
        return NFD_ERROR;
    }
    DBusMessage_Guard reply_guard(reply);

    // Check the reply and update our signal subscription if necessary
    {
        DBusMessageIter iter;
        if (!dbus_message_iter_init(reply, &iter)) {
            NFDi_SetError("D-Bus reply is missing an argument.");
            return NFD_ERROR;
        }
        if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_OBJECT_PATH) {
            NFDi_SetError("D-Bus reply is not an object path.");
            return NFD_ERROR;
        }

        const char* path;
        dbus_message_iter_get_basic(&iter, &path);
        if (strcmp(path, handle_obj_path) != 0) {
            // needs to change our signal subscription
            signal_sub.Subscribe(path);
        }
    }
    return NFD_OKAY;
}

char* ConvertToUriPath(const char* path)
{
    static constexpr char URI_FILE_PREFIX[] = "file://";

    char* uriPath = NFDi_Malloc<char>(sizeof(URI_FILE_PREFIX) / sizeof(char) + strlen(path));
    strcpy(uriPath, URI_FILE_PREFIX);
    strcpy(uriPath + sizeof(URI_FILE_PREFIX) - 1, path);

    return uriPath;
}

void AppendFileManagerParams(DBusMessage* query, const char* filePath)
{
    DBusMessageIter iter;
    dbus_message_iter_init_append(query, &iter);

    DBusMessageIter sub_iter;
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING_AS_STRING, &sub_iter);

    char* uriPath = ConvertToUriPath(filePath);

    dbus_message_iter_append_basic(&sub_iter, DBUS_TYPE_STRING, &uriPath);

    dbus_message_iter_close_container(&iter, &sub_iter);

    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &STR_EMPTY);

    NFDi_Free(uriPath);
}

nfdresult_t NFD_DBus_FileManager(const char* path, NfdFileManagerMode mode)
{
    static const char* method;
    if (mode == NFD_FM_OPEN_FOLDER)
        method = "ShowFolders";
    else if (mode == NFD_FM_SELECT_FILE)
        method = "ShowItems";
    else {
        NFDi_SetError("invalid NfdFileManagerMode");
        return NFD_ERROR;
    }

    DBusError err;  // need a separate error object because we don't want to mess with the old one
    // if it's still set
    dbus_error_init(&err);

    DBusMessage* query = dbus_message_new_method_call(
        "org.freedesktop.FileManager1",
        "/org/freedesktop/FileManager1",
        "org.freedesktop.FileManager1",
        method);

    DBusMessage_Guard query_guard(query);
    AppendFileManagerParams(query, path);

    DBusMessage* reply =
        dbus_connection_send_with_reply_and_block(dbus_conn, query, DBUS_TIMEOUT_INFINITE, &err);
    if (!reply) {
        dbus_error_free(&dbus_err);
        dbus_move_error(&err, &dbus_err);
        NFDi_SetError(dbus_err.message);
        return NFD_ERROR;
    }
    dbus_message_unref(reply);
    return NFD_OKAY;
}

template <bool Multiple, bool Directory>
nfdresult_t NFD_DBus_OpenFileWin(DBusMessage*& outMsg, NfdDialogParams* params)
{
    if (auto res = NFD_DBus_ShowOpenFileDialog<Multiple, Directory>(params); res != NFD_OKAY)
        return res;
    // Wait and read the response
    // const char* file = nullptr;
    do {
        while (true) {
            DBusMessage* msg = dbus_connection_pop_message(dbus_conn);
            if (!msg) break;

            if (dbus_message_is_signal(msg, "org.freedesktop.portal.Request", "Response")) {
                // this is the response we're looking for
                outMsg = msg;
                return NFD_OKAY;
            }

            dbus_message_unref(msg);
        }
    } while (dbus_connection_read_write(dbus_conn, -1));

    NFDi_SetError("D-Bus freedesktop portal did not give us a reply.");
    return NFD_ERROR;
}

// DBus wrapper function that helps invoke the portal for the SaveFile() API.
// This function returns NFD_OKAY iff outMsg gets set (to the returned message).
// Caller is responsible for freeing the outMsg using dbus_message_unref() (or use
// DBusMessage_Guard).
nfdresult_t NFD_DBus_SaveFile(DBusMessage*& outMsg,
                              const nfdnfilteritem_t* filterList,
                              nfdfiltersize_t filterCount,
                              const nfdnchar_t* defaultPath,
                              const nfdnchar_t* defaultName) {
    const char* handle_token_ptr;
    char* handle_obj_path = MakeUniqueObjectPath(&handle_token_ptr);
    Free_Guard<char> handle_obj_path_guard(handle_obj_path);

    DBusError err;  // need a separate error object because we don't want to mess with the old one
    // if it's stil set
    dbus_error_init(&err);

    // Subscribe to the signal using the handle_obj_path
    DBusSignalSubscriptionHandler signal_sub;
    nfdresult_t res = signal_sub.Subscribe(handle_obj_path);
    if (res != NFD_OKAY) return res;

    // TODO: use XOpenDisplay()/XGetInputFocus() to find xid of window... but what should one do on
    // Wayland?

    DBusMessage* query = dbus_message_new_method_call("org.freedesktop.portal.Desktop",
                                                      "/org/freedesktop/portal/desktop",
                                                      "org.freedesktop.portal.FileChooser",
                                                      "SaveFile");
    DBusMessage_Guard query_guard(query);
    AppendSaveFileQueryParams(
        query, handle_token_ptr, filterList, filterCount, defaultPath, defaultName);

    DBusMessage* reply =
        dbus_connection_send_with_reply_and_block(dbus_conn, query, DBUS_TIMEOUT_INFINITE, &err);
    if (!reply) {
        dbus_error_free(&dbus_err);
        dbus_move_error(&err, &dbus_err);
        NFDi_SetError(dbus_err.message);
        return NFD_ERROR;
    }
    DBusMessage_Guard reply_guard(reply);

    // Check the reply and update our signal subscription if necessary
    {
        DBusMessageIter iter;
        if (!dbus_message_iter_init(reply, &iter)) {
            NFDi_SetError("D-Bus reply is missing an argument.");
            return NFD_ERROR;
        }
        if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_OBJECT_PATH) {
            NFDi_SetError("D-Bus reply is not an object path.");
            return NFD_ERROR;
        }

        const char* path;
        dbus_message_iter_get_basic(&iter, &path);
        if (strcmp(path, handle_obj_path) != 0) {
            // needs to change our signal subscription
            signal_sub.Subscribe(path);
        }
    }

    // Wait and read the response
    // const char* file = nullptr;
    do {
        while (true) {
            DBusMessage* msg = dbus_connection_pop_message(dbus_conn);
            if (!msg) break;

            if (dbus_message_is_signal(msg, "org.freedesktop.portal.Request", "Response")) {
                // this is the response we're looking for
                outMsg = msg;
                return NFD_OKAY;
            }

            dbus_message_unref(msg);
        }
    } while (dbus_connection_read_write(dbus_conn, -1));

    NFDi_SetError("D-Bus freedesktop portal did not give us a reply.");
    return NFD_ERROR;
}

nfdresult_t NFD_DBus_ShowSaveFileDialog(NfdDialogParams* params)
{
    const char* handle_token_ptr;
    char* handle_obj_path = MakeUniqueObjectPath(&handle_token_ptr);
    Free_Guard<char> handle_obj_path_guard(handle_obj_path);

    DBusError err;  // need a separate error object because we don't want to mess with the old one
    // if it's stil set
    dbus_error_init(&err);

    // Subscribe to the signal using the handle_obj_path
    DBusSignalSubscriptionHandler signal_sub;
    nfdresult_t res = signal_sub.Subscribe(handle_obj_path);
    if (res != NFD_OKAY) return res;

    // TODO: use XOpenDisplay()/XGetInputFocus() to find xid of window... but what should one do on
    // Wayland?

    DBusMessage* query = dbus_message_new_method_call("org.freedesktop.portal.Desktop",
                                                      "/org/freedesktop/portal/desktop",
                                                      "org.freedesktop.portal.FileChooser",
                                                      "SaveFile");
    DBusMessage_Guard query_guard(query);
    AppendSaveFileQueryParams(query, handle_token_ptr, params);

    DBusMessage* reply =
        dbus_connection_send_with_reply_and_block(dbus_conn, query, DBUS_TIMEOUT_INFINITE, &err);
    if (!reply) {
        dbus_error_free(&dbus_err);
        dbus_move_error(&err, &dbus_err);
        NFDi_SetError(dbus_err.message);
        return NFD_ERROR;
    }
    DBusMessage_Guard reply_guard(reply);

    // Check the reply and update our signal subscription if necessary
    {
        DBusMessageIter iter;
        if (!dbus_message_iter_init(reply, &iter)) {
            NFDi_SetError("D-Bus reply is missing an argument.");
            return NFD_ERROR;
        }
        if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_OBJECT_PATH) {
            NFDi_SetError("D-Bus reply is not an object path.");
            return NFD_ERROR;
        }

        const char* path;
        dbus_message_iter_get_basic(&iter, &path);
        if (strcmp(path, handle_obj_path) != 0) {
            // needs to change our signal subscription
            signal_sub.Subscribe(path);
        }
    }
    return NFD_OKAY;
}

nfdresult_t NFD_DBus_SaveFileWin(DBusMessage*& outMsg, NfdDialogParams* params)
{
    if (auto res = NFD_DBus_ShowSaveFileDialog(params); res != NFD_OKAY)
        return res;

    // Wait and read the response
    // const char* file = nullptr;
    do {
        while (true) {
            DBusMessage* msg = dbus_connection_pop_message(dbus_conn);
            if (!msg) break;

            if (dbus_message_is_signal(msg, "org.freedesktop.portal.Request", "Response")) {
                // this is the response we're looking for
                outMsg = msg;
                return NFD_OKAY;
            }

            dbus_message_unref(msg);
        }
    } while (dbus_connection_read_write(dbus_conn, -1));

    NFDi_SetError("D-Bus freedesktop portal did not give us a reply.");
    return NFD_ERROR;
}

const char* formatRealpathError()
{
    switch (errno)
    {
        case EACCES:
            return "[realpath] Search permission was denied for a component of the path prefix of file_name.";
        case EINVAL:
            return "[realpath] The file_name argument is a null pointer.";
        case EIO:
            return "[realpath] An error occurred while reading from the file system.";
        case ELOOP:
            return "[realpath] A loop exists in symbolic links encountered during resolution of the file_name argument.";
        case ENAMETOOLONG:
            return "[realpath] The length of a component of a pathname is longer than {NAME_MAX}.";
        case ENOENT:
            return "[realpath] A component of file_name does not name an existing file or file_name points to an empty string.";
        case ENOTDIR:
            return "[realpath] A component of the path prefix names an existing file that is neither a directory nor a symbolic link to a directory, or the file_name argument contains at least one non- <slash> character and ends with one or more trailing <slash> characters and the last pathname component names an existing file that is neither a directory nor a symbolic link to a directory.";
        default:
            return "[realpath] unknown error.";
    }
}

}  // namespace

/* public */

const char* NFD_GetError(void) {
    return err_ptr;
}

void NFD_ClearError(void) {
    NFDi_SetError(nullptr);
    dbus_error_free(&dbus_err);
}

nfdresult_t NFD_Init(void) {
    // Initialize dbus_error
    dbus_error_init(&dbus_err);
    // Get DBus connection
    dbus_conn = dbus_bus_get_private(DBUS_BUS_SESSION, &dbus_err);
    if (!dbus_conn) {
        NFDi_SetError(dbus_err.message);
        return NFD_ERROR;
    }
    dbus_connection_set_exit_on_disconnect(dbus_conn, false);
    dbus_unique_name = dbus_bus_get_unique_name(dbus_conn);
    if (!dbus_unique_name) {
        NFDi_SetError("Unable to get the unique name of our D-Bus connection.");
        return NFD_ERROR;
    }

    return NFD_OKAY;
}
void NFD_Quit(void) {
    dbus_connection_close(dbus_conn);
    dbus_connection_unref(dbus_conn);
    // Note: We do not free dbus_error since NFD_Init might set it.
    // To avoid leaking memory, the caller should explicitly call NFD_ClearError after reading the
    // error.
}

void NFD_FreePathN(nfdnchar_t* filePath) {
    NFDi_Free(filePath);
}

nfdresult_t NFD_OpenDialogN(nfdnchar_t** outPath,
                            const nfdnfilteritem_t* filterList,
                            nfdfiltersize_t filterCount,
                            const nfdnchar_t* defaultPath) {
    (void)defaultPath;  // Default path not supported for portal backend

    DBusMessage* msg;
    {
        const nfdresult_t res = NFD_DBus_OpenFile<false, false>(msg, filterList, filterCount);
        if (res != NFD_OKAY) {
            return res;
        }
    }
    DBusMessage_Guard msg_guard(msg);

    const char* uri;
    {
        const nfdresult_t res = ReadResponseUrisSingle(msg, uri);
        if (res != NFD_OKAY) {
            return res;
        }
    }

    return AllocAndCopyFilePath(uri, *outPath);
}

nfdresult_t NFD_OpenDialogWin(NfdDialogParams* params)
{
    (void)params->defaultPath;  // Default path not supported for portal backend
    if (params->outAsyncOpHandle)
    {
        nfdresult_t res = NFD_DBus_ShowOpenFileDialog<false, false>(params);
        if (res != NFD_OKAY)
            return res;
        NfdDialogMonitor* monitor = NfdDialogMonitor::create<false>();
        if (!monitor)
            return NFD_ERROR;
        *params->outAsyncOpHandle = monitor;

        return NFD_OKAY;
    }
    else
    {
        DBusMessage* msg;
        {
            const nfdresult_t res = NFD_DBus_OpenFileWin<false, false>(msg, params);
            if (res != NFD_OKAY) {
                return res;
            }
        }
        DBusMessage_Guard msg_guard(msg);

        const char* uri;
        {
            const nfdresult_t res = ReadResponseUrisSingle(msg, uri);
            if (res != NFD_OKAY) {
                return res;
            }
        }

        return AllocAndCopyFilePath(uri, *params->outPath, &params->outPathSize);
    }
}

nfdresult_t NFD_OpenFileManager(NfdFileManagerParams* params)
{
    if (params->convertToRealPath) {
        char* path = realpath(params->filePath, nullptr);
        if (!path) {
            NFDi_SetError(formatRealpathError());
            return NFD_ERROR;
        }
        auto ret = NFD_DBus_FileManager(path, params->mode);

        free(path);

        return ret;
    }
    else
        return NFD_DBus_FileManager(params->filePath, params->mode);

}

nfdresult_t NFD_OpenDialogMultipleWin(NfdDialogParams* params)
{
    (void)params->defaultPath;  // Default path not supported for portal backend

    if (params->outAsyncOpHandle)
    {
        nfdresult_t res = NFD_DBus_ShowOpenFileDialog<true, false>(params);
        if (res != NFD_OKAY)
            return res;
        NfdDialogMonitor* monitor = NfdDialogMonitor::create<true>();
        if (!monitor)
            return NFD_ERROR;
        *params->outAsyncOpHandle = monitor;

        return NFD_OKAY;
    }
    else
    {
        DBusMessage* msg;
        {
            const nfdresult_t res = NFD_DBus_OpenFileWin<true, false>(msg, params);
            if (res != NFD_OKAY) {
                return res;
            }
        }

        DBusMessageIter uri_iter;
        nfdresult_t res = ReadResponseUris(msg, uri_iter);
        if (res != NFD_OKAY) {
            dbus_message_unref(msg);
            return res;
        }

        nfdpathsetsize_t numPaths;
        NFD_PathSet_GetCount(msg, &numPaths);

        int pathListSize = 256;
        *params->outPath = NFDi_Malloc<char>(pathListSize);
        char* curOutPath = *params->outPath;

        if (numPaths == 1)
            res = NFD_PathSet_GetPathWin<policy::FullPath>(
                msg, 0, *params->outPath, curOutPath, pathListSize);
        else
            res = NFD_PathSet_GetPathWin<policy::Dirname>(
                msg, 0, *params->outPath, curOutPath, pathListSize);

        if (res != NFD_OKAY) {
            NFDi_Free(*params->outPath);
            *params->outPath = nullptr;
            return NFD_ERROR;
        }

        for (nfdpathsetsize_t i = 1; i < numPaths; ++i) {
            if (NFD_PathSet_GetPathWin<policy::Basename>(
                    msg, i, *params->outPath, curOutPath, pathListSize) != NFD_OKAY) {
                NFDi_Free(*params->outPath);
                *params->outPath = nullptr;
                return NFD_ERROR;
            }
        }

        if (pathListSize == curOutPath - *params->outPath) {
            ptrdiff_t offset = curOutPath - *params->outPath;
            *params->outPath =
                NFDi_Realloc<char>(*params->outPath, ++pathListSize);
            curOutPath = *params->outPath + offset;
        }
        *curOutPath = '\0';  // double null-terminate
        params->outPathSize = curOutPath - *params->outPath + 1;

        return NFD_OKAY;
    }
}

nfdresult_t NFD_OpenDialogMultipleN(const nfdpathset_t** outPaths,
                                    const nfdnfilteritem_t* filterList,
                                    nfdfiltersize_t filterCount,
                                    const nfdnchar_t* defaultPath) {
    (void)defaultPath;  // Default path not supported for portal backend

    DBusMessage* msg;
    {
        const nfdresult_t res = NFD_DBus_OpenFile<true, false>(msg, filterList, filterCount);
        if (res != NFD_OKAY) {
            return res;
        }
    }

    DBusMessageIter uri_iter;
    const nfdresult_t res = ReadResponseUris(msg, uri_iter);
    if (res != NFD_OKAY) {
        dbus_message_unref(msg);
        return res;
    }

    *outPaths = msg;
    return NFD_OKAY;
}

nfdresult_t NFD_SaveDialogWin(NfdDialogParams* params)
{
    if (params->outAsyncOpHandle)
    {
        nfdresult_t res = NFD_DBus_ShowSaveFileDialog(params);
        if (res != NFD_OKAY)
            return res;
        NfdDialogMonitor* monitor = NfdDialogMonitor::create<false>();
        if (!monitor)
            return NFD_ERROR;
        *params->outAsyncOpHandle = monitor;

        return NFD_OKAY;
    }
    else
    {
        DBusMessage* msg;
        {
            const nfdresult_t res = NFD_DBus_SaveFileWin(msg, params);
            if (res != NFD_OKAY) {
                return res;
            }
        }
        DBusMessage_Guard msg_guard(msg);

#ifdef NFD_APPEND_EXTENSION
        const char* uri;
        const char* extn;
        {
            const nfdresult_t res = ReadResponseUrisSingleAndCurrentExtension(msg, uri, extn);
            if (res != NFD_OKAY) {
                return res;
            }
        }

        return AllocAndCopyFilePathWithExtn(uri, extn, *outPath);
#else
        const char* uri;
        {
            const nfdresult_t res = ReadResponseUrisSingle(msg, uri);
            if (res != NFD_OKAY) {
                return res;
            }
        }

        return AllocAndCopyFilePath(uri, *params->outPath, &params->outPathSize);
#endif
    }
}

int NFD_HasAsyncOpCompleted(void* opHandle)
{
    if (!opHandle) {
        NFDi_SetError("opHandle null");
        return 0;
    }
    // assume the caller passed in the correct opHandle
    return static_cast<NfdDialogMonitor*>(opHandle)->hasDialogReturned();
}

nfdresult_t NFD_GetAsyncOpResult(void* opHandle, NfdDialogResponse* result)
{
    if (!opHandle) {
        NFDi_SetError("opHandle null");
        return NFD_ERROR;
    }
    // assume the caller passed in the correct opHandle
    return static_cast<NfdDialogMonitor*>(opHandle)->getDialogResult(result);
}

void NFD_FreeHandle(void* opHandle)
{
    NFDi_Free(opHandle);
}

nfdresult_t NFD_SaveDialogN(nfdnchar_t** outPath,
                            const nfdnfilteritem_t* filterList,
                            nfdfiltersize_t filterCount,
                            const nfdnchar_t* defaultPath,
                            const nfdnchar_t* defaultName) {
    DBusMessage* msg;
    {
        const nfdresult_t res =
            NFD_DBus_SaveFile(msg, filterList, filterCount, defaultPath, defaultName);
        if (res != NFD_OKAY) {
            return res;
        }
    }
    DBusMessage_Guard msg_guard(msg);

#ifdef NFD_APPEND_EXTENSION
    const char* uri;
    const char* extn;
    {
        const nfdresult_t res = ReadResponseUrisSingleAndCurrentExtension(msg, uri, extn);
        if (res != NFD_OKAY) {
            return res;
        }
    }

    return AllocAndCopyFilePathWithExtn(uri, extn, *outPath);
#else
    const char* uri;
    {
        const nfdresult_t res = ReadResponseUrisSingle(msg, uri);
        if (res != NFD_OKAY) {
            return res;
        }
    }

    return AllocAndCopyFilePath(uri, *outPath);
#endif
}

nfdresult_t NFD_PickFolderN(nfdnchar_t** outPath, const nfdnchar_t* defaultPath) {
    (void)defaultPath;  // Default path not supported for portal backend

    DBusMessage* msg;
    {
        const nfdresult_t res = NFD_DBus_OpenFile<false, true>(msg, nullptr, 0);
        if (res != NFD_OKAY) {
            return res;
        }
    }
    DBusMessage_Guard msg_guard(msg);

    const char* uri;
    {
        const nfdresult_t res = ReadResponseUrisSingle(msg, uri);
        if (res != NFD_OKAY) {
            return res;
        }
    }

    return AllocAndCopyFilePath(uri, *outPath);
}

nfdresult_t NFD_PathSet_GetCount(const nfdpathset_t* pathSet, nfdpathsetsize_t* count) {
    assert(pathSet);
    DBusMessage* msg = const_cast<DBusMessage*>(static_cast<const DBusMessage*>(pathSet));
    *count = ReadResponseUrisUncheckedGetArraySize(msg);
    return NFD_OKAY;
}

nfdresult_t NFD_PathSet_GetPathN(const nfdpathset_t* pathSet,
                                 nfdpathsetsize_t index,
                                 nfdnchar_t** outPath) {
    assert(pathSet);
    DBusMessage* msg = const_cast<DBusMessage*>(static_cast<const DBusMessage*>(pathSet));
    DBusMessageIter uri_iter;
    ReadResponseUrisUnchecked(msg, uri_iter);
    while (index > 0) {
        --index;
        if (!dbus_message_iter_next(&uri_iter)) {
            NFDi_SetError("Index out of bounds.");
            return NFD_ERROR;
        }
    }
    if (dbus_message_iter_get_arg_type(&uri_iter) != DBUS_TYPE_STRING) {
        NFDi_SetError("D-Bus response signal URI sub iter is not a string.");
        return NFD_ERROR;
    }
    const char* uri;
    dbus_message_iter_get_basic(&uri_iter, &uri);
    return AllocAndCopyFilePath(uri, *outPath);
}

void NFD_PathSet_FreePathN(const nfdnchar_t* filePath) {
    assert(filePath);
    NFD_FreePathN(const_cast<nfdnchar_t*>(filePath));
}

void NFD_PathSet_Free(const nfdpathset_t* pathSet) {
    assert(pathSet);
    DBusMessage* msg = const_cast<DBusMessage*>(static_cast<const DBusMessage*>(pathSet));
    dbus_message_unref(msg);
}

nfdresult_t NFD_PathSet_GetEnum(const nfdpathset_t* pathSet, nfdpathsetenum_t* outEnumerator) {
    assert(pathSet);
    DBusMessage* msg = const_cast<DBusMessage*>(static_cast<const DBusMessage*>(pathSet));
    ReadResponseUrisUnchecked(msg, *reinterpret_cast<DBusMessageIter*>(outEnumerator));
    return NFD_OKAY;
}

void NFD_PathSet_FreeEnum(nfdpathsetenum_t*) {
    // Do nothing, because the enumeration is just a message iterator
}

nfdresult_t NFD_PathSet_EnumNextN(nfdpathsetenum_t* enumerator, nfdnchar_t** outPath) {
    DBusMessageIter& uri_iter = *reinterpret_cast<DBusMessageIter*>(enumerator);
    const int arg_type = dbus_message_iter_get_arg_type(&uri_iter);
    if (arg_type == DBUS_TYPE_INVALID) {
        *outPath = nullptr;
        return NFD_OKAY;
    }
    if (arg_type != DBUS_TYPE_STRING) {
        NFDi_SetError("D-Bus response signal URI sub iter is not a string.");
        return NFD_ERROR;
    }
    const char* uri;
    dbus_message_iter_get_basic(&uri_iter, &uri);
    const nfdresult_t res = AllocAndCopyFilePath(uri, *outPath);
    if (res != NFD_OKAY) return res;
    dbus_message_iter_next(&uri_iter);
    return NFD_OKAY;
}
