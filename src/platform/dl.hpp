#pragma once
#include <utility>

#include <dlfcn.h>

namespace mirage::platform {

class dl_lib {
public:
    static dl_lib open(const char* name) { return dl_lib{dlopen(name, RTLD_NOW | RTLD_LOCAL)}; }

    [[nodiscard]] bool loaded() const { return handle_ != nullptr; }

    template <typename F>
    [[nodiscard]] F sym(const char* name) const {
        return reinterpret_cast<F>(dlsym(handle_, name));
    }

    dl_lib(dl_lib&& o) noexcept : handle_(std::exchange(o.handle_, nullptr)) {}

    dl_lib& operator=(dl_lib&& o) noexcept {
        if (this != &o) {
            if (handle_) {
                dlclose(handle_);
            }
            handle_ = std::exchange(o.handle_, nullptr);
        }
        return *this;
    }

    ~dl_lib() {
        if (handle_) {
            dlclose(handle_);
        }
    }

    dl_lib(const dl_lib&) = delete;
    dl_lib& operator=(const dl_lib&) = delete;

private:
    explicit dl_lib(void* h) : handle_(h) {}
    void* handle_;
};

}  // namespace mirage::platform
