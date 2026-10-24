// SPDX-License-Identifier: GPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "common.hpp"
#include "kaux.h" /* the wire format, shared with the module */

namespace uidfake {

/*
 * The bytes of one stream (include/kaux.h): BEGIN says what the stream carries,
 * every BATCH is a record count followed by that many whole records, and END
 * finishes it. Everything is little endian and a multiple of four bytes long,
 * exactly as src/netlink.c reads it.
 */

/* One message carries at most this many payload bytes (KAUX_BATCH_BYTES in
 * src/netlink.c); a batch is split so that no message goes over it. */
inline constexpr std::size_t kMessageBytes = 32768;

/* A policy record is op, caller, target, flags. */
inline constexpr std::size_t kPolicyRecWords = KAUX_POLICY_REC_WORDS;
/* An apk record is op, uid, dev, ino_low, ino_high, path_bytes, then the path
 * bytes zero padded to a four byte boundary. */
inline constexpr std::size_t kApkRecWords = KAUX_APK_REC_WORDS;
/* The token stream's batch is the count and the four words of the name. */
inline constexpr std::size_t kTokenWords = 4;

/* BEGIN: struct kaux_begin, the kind of set this stream carries. */
[[nodiscard]] std::vector<std::byte> begin_payload(std::uint32_t kind);

/* The record count a batch starts with. */
[[nodiscard]] std::vector<std::byte> batch_header(std::uint32_t count);

/* One policy record: the caller, its target, and the verdict's own bits. */
[[nodiscard]] std::vector<std::byte> policy_record(std::uint32_t caller,
                                                   std::uint32_t target);

/* One apk record, with the path bytes it names. A drop carries no path: the
 * file is gone, and the module's ledger only has (dev, ino) left for it. */
[[nodiscard]] std::vector<std::byte>
apk_record(std::uint32_t op, std::uint32_t uid, std::uint32_t dev,
           std::uint64_t ino, std::string_view path);

/* The token stream's one batch: the count, then the name as four words. */
[[nodiscard]] std::vector<std::byte> token_batch(const std::uint32_t *words);

/* st_dev as stat(2) reports it: the encoding the module compares against, which
 * is cp_new_stat()'s own. */
[[nodiscard]] std::uint32_t encode_dev(std::uint64_t st_dev);

} // namespace uidfake
