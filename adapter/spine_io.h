#pragma once

#include <spine/Extension.h>
#include <spine/SpineString.h>
#include <cstdio>

#include "pathio.h"

namespace sl {

class WidePathExtension : public spine::DefaultSpineExtension {
public:
    virtual ~WidePathExtension() {}

protected:

    virtual char *_readFile(const spine::String &path, int *length) override {
        *length = 0;
        if (!path.buffer()) return nullptr;

        const std::wstring wide = widenUtf8(path.buffer());
        if (wide.empty()) return nullptr;

        FILE *file = nullptr;
        if (_wfopen_s(&file, wide.c_str(), L"rb") != 0 || !file) return nullptr;

        if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return nullptr; }
        const long size = ftell(file);
        if (size < 0) { fclose(file); return nullptr; }
        rewind(file);

        char *data = spine::SpineExtension::alloc<char>((size_t)size, __FILE__, __LINE__);
        if (!data) { fclose(file); return nullptr; }

        const size_t got = fread(data, 1, (size_t)size, file);
        fclose(file);
        if (got != (size_t)size) {
            spine::SpineExtension::free(data, __FILE__, __LINE__);
            return nullptr;
        }

        *length = (int)size;
        return data;
    }
};

}
