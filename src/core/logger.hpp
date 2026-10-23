// Leveled logging to stderr, which systemd hands to the journal.

#pragma once

#include <cstdarg>
#include <cstdio>
#include <string>

namespace esca::capability::log {

enum class Level { Debug = 0, Info, Warn, Error };

/// Process-wide; the service sets it from its configuration.
Level &threshold();

bool set_threshold(const std::string &name);

void write(Level level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

} // namespace esca::capability::log

#define ESCA_LOG_DEBUG(...) ::esca::capability::log::write(::esca::capability::log::Level::Debug, __VA_ARGS__)
#define ESCA_LOG_INFO(...) ::esca::capability::log::write(::esca::capability::log::Level::Info, __VA_ARGS__)
#define ESCA_LOG_WARN(...) ::esca::capability::log::write(::esca::capability::log::Level::Warn, __VA_ARGS__)
#define ESCA_LOG_ERROR(...) ::esca::capability::log::write(::esca::capability::log::Level::Error, __VA_ARGS__)
