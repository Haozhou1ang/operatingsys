// server/include/storage/storage.h
#pragma once

#include <mutex>
#include <shared_mutex>

// Global storage lock shared by all services.
// Read paths should take shared_lock; write/transaction paths take unique_lock.
// The server is multi-threaded (one thread per client connection).
extern std::shared_mutex g_fs_mu;
