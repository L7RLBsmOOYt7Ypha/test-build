// SPDX-License-Identifier: GPL-2.0
#include "paging.hpp"

#include <cstring>

namespace uidfake {
namespace {

/* Little endian, matching the kernel side (see src/netlink.c). */
void put_u32(std::byte *out, std::size_t index, std::uint32_t value) {
  for (std::uint32_t i = 0; i < 4; i++)
    out[index + i] = static_cast<std::byte>((value >> (8 * i)) & 0xffu);
}

void put_u64(std::byte *out, std::size_t index, std::uint64_t value) {
  put_u32(out, index, static_cast<std::uint32_t>(value & 0xffffffffu));
  put_u32(out, index + 4, static_cast<std::uint32_t>(value >> 32));
}

} // namespace

std::uint32_t encode_dev(std::uint64_t st_dev) {
  /* stat(2) hands out the encoding cp_new_stat() writes, which is the one
   * tosya_encode_dev() produces on the kernel side -- the two are the same
   * layout, so the value travels as it stands. */
  return static_cast<std::uint32_t>(st_dev);
}

std::vector<std::byte> begin_payload(std::uint32_t kind) {
  std::vector<std::byte> out(4); /* struct kaux_begin: the kind alone */
  put_u32(out.data(), 0, kind);
  return out;
}

std::vector<std::byte> batch_header(std::uint32_t count) {
  std::vector<std::byte> out(4);
  put_u32(out.data(), 0, count);
  return out;
}

std::vector<std::byte> policy_record(std::uint32_t caller,
                                     std::uint32_t target) {
  std::vector<std::byte> out(kPolicyRecWords * 4);
  put_u32(out.data(), 0, KAUX_OP_ADD);
  put_u32(out.data(), 4, caller);
  put_u32(out.data(), 8, target);
  put_u32(out.data(), 12, 0); /* flags: the module reads none of them */
  return out;
}

std::vector<std::byte> apk_record(std::uint32_t op, std::uint32_t uid,
                                  std::uint32_t dev, std::uint64_t ino,
                                  std::string_view path) {
  const std::size_t padded = (path.size() + 3u) & ~std::size_t{3};
  std::vector<std::byte> out(kApkRecWords * 4 + padded);

  put_u32(out.data(), 0, op);
  put_u32(out.data(), 4, uid);
  put_u32(out.data(), 8, dev);
  put_u64(out.data(), 12, ino);
  put_u32(out.data(), 20, static_cast<std::uint32_t>(path.size()));
  if (!path.empty())
    std::memcpy(out.data() + kApkRecWords * 4, path.data(), path.size());
  return out;
}

std::vector<std::byte> token_batch(const std::uint32_t *words) {
  std::vector<std::byte> out(4 + kTokenWords * 4);

  put_u32(out.data(), 0, kTokenWords);
  for (std::size_t i = 0; i < kTokenWords; i++)
    put_u32(out.data(), 4 + 4 * i, words[i]);
  return out;
}

} // namespace uidfake
