// SPDX-License-Identifier: GPL-2.0
#include "netlink.hpp"
#include "status.hpp"

#include "paging.hpp"

#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <cstring>
#include <vector>

namespace uidfake {
namespace {

/*
 * The NDK UAPI headers only ship NLA_ALIGN/NLA_HDRLEN plus macros that do not
 * handle const pointers, so iterate explicitly with a byte cursor and these two
 * predicates.
 */
[[nodiscard]] bool nlmsg_ok(const nlmsghdr *header, int left) {
  return left >= static_cast<int>(sizeof(nlmsghdr)) &&
         header->nlmsg_len >= sizeof(nlmsghdr) &&
         static_cast<int>(header->nlmsg_len) <= left;
}

[[nodiscard]] bool nlattr_ok(const nlattr *attr, int left) {
  return left >= static_cast<int>(sizeof(nlattr)) &&
         attr->nla_len >= sizeof(nlattr) &&
         static_cast<int>(attr->nla_len) <= left;
}

[[nodiscard]] const nlmsghdr *nlmsg_next(const nlmsghdr *header) {
  return reinterpret_cast<const nlmsghdr *>(
      reinterpret_cast<const std::byte *>(header) +
      NLMSG_ALIGN(header->nlmsg_len));
}

[[nodiscard]] const nlattr *nlattr_next(const nlattr *attr) {
  return reinterpret_cast<const nlattr *>(
      reinterpret_cast<const std::byte *>(attr) + NLA_ALIGN(attr->nla_len));
}

/* A netlink buffer with typed views into its header area. */
struct Buffer {
  std::vector<std::byte> bytes;

  [[nodiscard]] nlmsghdr *nlmsg() {
    return reinterpret_cast<nlmsghdr *>(bytes.data());
  }
  [[nodiscard]] genlmsghdr *genlmsg() {
    return reinterpret_cast<genlmsghdr *>(bytes.data() + NLMSG_HDRLEN);
  }
};

} // namespace

bool NetlinkClient::exchange(std::span<const std::byte> request,
                             std::span<std::byte> reply) {
  sockaddr_nl address{};
  address.nl_family = AF_NETLINK;

  iovec iov{.iov_base = const_cast<std::byte *>(request.data()),
            .iov_len = request.size()};
  msghdr message{};
  message.msg_name = &address;
  message.msg_namelen = sizeof(address);
  message.msg_iov = &iov;
  message.msg_iovlen = 1;

  if (::sendmsg(socket_.get(), &message, 0) < 0) {
    Log::warn("sendmsg: {}", std::strerror(errno));
    return false;
  }

  iov.iov_base = reply.data();
  iov.iov_len = reply.size();
  const ssize_t received = ::recvmsg(socket_.get(), &message, 0);
  if (received < 0) {
    /* EAGAIN is the read deadline firing: the module is gone or never answered.
     */
    Log::warn("recvmsg: {}", errno == EAGAIN
                                 ? "timed out waiting for the kernel"
                                 : std::strerror(errno));
    return false;
  }

  reply_bytes_ = static_cast<std::size_t>(received);
  const auto *header = reinterpret_cast<const nlmsghdr *>(reply.data());
  bool answered = false;
  for (int left = static_cast<int>(received); nlmsg_ok(header, left);) {
    const int step = static_cast<int>(NLMSG_ALIGN(header->nlmsg_len));
    if (header->nlmsg_seq != seq_) {
      /* A late reply to a request that already timed out; ignore it. */
      left -= step;
      header = nlmsg_next(header);
      continue;
    }
    if (header->nlmsg_type == NLMSG_ERROR) {
      const auto *error =
          reinterpret_cast<const nlmsgerr *>(NLMSG_DATA(header));
      if (error->error != 0) {
        /* A module older than this tool does not know the command; that is
         * worth telling apart from a module that is not there at all. */
        if (error->error == -EOPNOTSUPP || error->error == -EPROTONOSUPPORT)
          unsupported_ = true;
        last_error_ = error->error;
        Log::warn("netlink error: {}", std::strerror(-error->error));
        return false;
      }
    }
    answered = true;
    left -= step;
    header = nlmsg_next(header);
  }
  return answered;
}

std::optional<std::uint16_t> NetlinkClient::resolve_family() {
  if (!ensure_connected())
    return std::nullopt;

  Buffer request{};
  const std::size_t name_len = kFamilyName.size() + 1;
  request.bytes.assign(NLMSG_SPACE(GENL_HDRLEN) + NLA_HDRLEN + name_len,
                       std::byte{0});

  auto *nlh = request.nlmsg();
  nlh->nlmsg_len =
      NLMSG_LENGTH(GENL_HDRLEN + NLA_HDRLEN + static_cast<int>(name_len));
  nlh->nlmsg_type = GENL_ID_CTRL;
  nlh->nlmsg_flags = NLM_F_REQUEST;
  nlh->nlmsg_seq = ++seq_;

  auto *genl = request.genlmsg();
  genl->cmd = CTRL_CMD_GETFAMILY;
  genl->version = 1;

  auto *attr = reinterpret_cast<nlattr *>(reinterpret_cast<std::byte *>(genl) +
                                          GENL_HDRLEN);
  attr->nla_type = CTRL_ATTR_FAMILY_NAME;
  attr->nla_len = NLA_HDRLEN + static_cast<int>(name_len);
  std::memcpy(reinterpret_cast<std::byte *>(attr) + NLA_HDRLEN,
              kFamilyName.data(), name_len);

  std::vector<std::byte> reply(kReplySize);
  if (!exchange(request.bytes, reply))
    return std::nullopt;

  const auto *header = reinterpret_cast<const nlmsghdr *>(reply.data());
  for (int left = static_cast<int>(reply.size()); nlmsg_ok(header, left);) {
    if (header->nlmsg_type == NLMSG_ERROR)
      break;

    const auto *genl = reinterpret_cast<const genlmsghdr *>(NLMSG_DATA(header));
    int attr_left = static_cast<int>(header->nlmsg_len) -
                    static_cast<int>(NLMSG_LENGTH(GENL_HDRLEN));
    const auto *attr = reinterpret_cast<const nlattr *>(
        reinterpret_cast<const std::byte *>(genl) + GENL_HDRLEN);
    while (nlattr_ok(attr, attr_left)) {
      if (attr->nla_type == CTRL_ATTR_FAMILY_ID) {
        std::uint16_t id = 0;
        std::memcpy(&id, reinterpret_cast<const std::byte *>(attr) + NLA_HDRLEN,
                    sizeof(id));
        return id;
      }
      attr_left -= static_cast<int>(NLA_ALIGN(attr->nla_len));
      attr = nlattr_next(attr);
    }

    left -= static_cast<int>(NLMSG_ALIGN(header->nlmsg_len));
    header = nlmsg_next(header);
  }

  if (!warned_) {
    warned_ = true;
    Log::warn("generic netlink family {} not found (module not loaded?)",
              kFamilyName);
  }
  return std::nullopt;
}

bool NetlinkClient::ensure_connected() {
  if (socket_.valid())
    return true;

  const int fd = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
  if (fd < 0) {
    Log::warn("socket: {}", std::strerror(errno));
    return false;
  }

  sockaddr_nl address{};
  address.nl_family = AF_NETLINK;
  if (::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
    Log::warn("bind: {}", std::strerror(errno));
    ::close(fd);
    return false;
  }

  /*
   * Both directions need a deadline. If the module is unloaded between our
   * request and the reply there is nothing to receive, and without one the
   * helper would sit in recvmsg() forever: that is the "cannot connect after
   * rmmod/insmod" symptom.
   */
  timeval deadline{.tv_sec = 0, .tv_usec = 500'000};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &deadline, sizeof(deadline));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &deadline, sizeof(deadline));

  socket_.reset(fd);
  return true;
}

void NetlinkClient::note_reachable() {
  if (!warned_)
    return;
  warned_ = false;
  Log::info("kernel side reachable again");
}

bool NetlinkClient::send_command(std::uint8_t cmd,
                                 std::span<const std::byte> blob) {
  if (!ensure_connected())
    return false;

  if (!family_) {
    const auto resolved = resolve_family();
    if (!resolved)
      return false;
    family_ = resolved;
  }

  Buffer request{};
  request.bytes.assign(NLMSG_SPACE(GENL_HDRLEN) +
                           NLA_ALIGN(NLA_HDRLEN + blob.size()),
                       std::byte{0});

  auto *nlh = request.nlmsg();
  nlh->nlmsg_len =
      NLMSG_LENGTH(GENL_HDRLEN + NLA_HDRLEN + static_cast<int>(blob.size()));
  nlh->nlmsg_type = *family_;
  nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  nlh->nlmsg_seq = ++seq_;

  auto *genl = request.genlmsg();
  genl->cmd = cmd;
  genl->version = KAUX_FAMILY_VERSION;

  auto *attr = reinterpret_cast<nlattr *>(reinterpret_cast<std::byte *>(genl) +
                                          GENL_HDRLEN);
  attr->nla_type = KAUX_ATTR_BLOB;
  attr->nla_len = NLA_HDRLEN + static_cast<int>(blob.size());
  std::memcpy(reinterpret_cast<std::byte *>(attr) + NLA_HDRLEN, blob.data(),
              blob.size());

  std::vector<std::byte> reply(kReplySize);
  return exchange(std::span{request.bytes}.first(nlh->nlmsg_len), reply);
}

bool NetlinkClient::send_stream(
    std::uint32_t kind, const std::vector<std::vector<std::byte>> &records) {
  if (!send_command(KAUX_CMD_BEGIN, begin_payload(kind)))
    return false;

  std::vector<std::byte> batch; /* the records of the batch in progress */
  std::uint32_t count = 0;
  auto flush = [&]() -> bool {
    if (count == 0)
      return true;
    std::vector<std::byte> payload = batch_header(count);
    payload.insert(payload.end(), batch.begin(), batch.end());
    const bool ok = send_command(KAUX_CMD_BATCH, payload);
    batch.clear();
    count = 0;
    return ok;
  };

  for (const auto &record : records) {
    /* A batch is a count and whole records: one that does not fit a message on
     * its own is refused here, with the size that was too much in the log. */
    if (record.size() + 4 > kMessageBytes) {
      Log::warn("one record of {} byte(s) does not fit a message (cap {})",
                record.size(), kMessageBytes);
      return false;
    }
    if (count > 0 && 4 + batch.size() + record.size() > kMessageBytes &&
        !flush())
      return false;
    batch.insert(batch.end(), record.begin(), record.end());
    count++;
  }
  if (!flush())
    return false;

  /*
   * END answers with what the stream did. -EIO says some of its records did not
   * land: that is an answer and not an outage -- what landed stays in the
   * kernel's ledger, and the status line counts what did not. Anything else is
   * a refusal, and the caller keeps what it had.
   */
  if (!send_command(KAUX_CMD_END, {}) && last_error_ != -EIO)
    return false;
  return true;
}

bool NetlinkClient::push(std::span<const Pair> pairs) {
  /* The whole policy goes up as one stream and becomes live at its END: a
   * policy that arrives half way never takes effect, and an empty list is the
   * one that clears it. */
  std::vector<std::vector<std::byte>> records;
  records.reserve(pairs.size());
  for (const auto &pair : pairs)
    records.push_back(policy_record(pair.caller, pair.target));

  for (int attempt = 0; attempt < 2; ++attempt) {
    if (send_stream(KAUX_KIND_POLICY, records)) {
      note_reachable();
      return true;
    }
    family_.reset();
    socket_.reset();
  }
  if (!warned_) {
    warned_ = true;
    Log::warn("kernel side unreachable, keeping the previous policy");
    report_status("kernel unreachable");
  }
  return false;
}

std::optional<kaux_status> NetlinkClient::status() {
  if (!ensure_connected())
    return std::nullopt;
  if (!family_) {
    const auto resolved = resolve_family();
    if (!resolved)
      return std::nullopt;
    family_ = resolved;
  }

  Buffer request{};
  request.bytes.assign(NLMSG_SPACE(GENL_HDRLEN), std::byte{0});
  auto *nlh = request.nlmsg();
  nlh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
  nlh->nlmsg_type = *family_;
  nlh->nlmsg_flags = NLM_F_REQUEST;
  nlh->nlmsg_seq = ++seq_;
  request.genlmsg()->cmd = KAUX_CMD_STATUS;
  request.genlmsg()->version = KAUX_FAMILY_VERSION;

  std::vector<std::byte> reply(kReplySize);
  unsupported_ = false;
  if (!exchange(std::span{request.bytes}.first(nlh->nlmsg_len), reply)) {
    if (unsupported_)
      Log::warn("the loaded module does not know KAUX_CMD_STATUS; it is older "
                "than this tool (a reboot loads the new one)");
    return std::nullopt;
  }

  const auto view = std::span<const std::byte>(reply.data(), reply_bytes_);
  int left = static_cast<int>(view.size());
  for (const std::byte *cursor = view.data();
       nlmsg_ok(reinterpret_cast<const nlmsghdr *>(cursor), left);) {
    const auto *header = reinterpret_cast<const nlmsghdr *>(cursor);
    const int step = static_cast<int>(NLMSG_ALIGN(header->nlmsg_len));
    if (header->nlmsg_type != NLMSG_ERROR && header->nlmsg_seq == seq_) {
      const auto *genl =
          reinterpret_cast<const genlmsghdr *>(NLMSG_DATA(header));
      int attr_left = static_cast<int>(header->nlmsg_len) -
                      static_cast<int>(NLMSG_LENGTH(GENL_HDRLEN));
      const auto *attr = reinterpret_cast<const nlattr *>(
          reinterpret_cast<const std::byte *>(genl) + GENL_HDRLEN);
      for (; nlattr_ok(attr, attr_left); attr = nlattr_next(attr)) {
        const int alen = static_cast<int>(attr->nla_len) - NLA_HDRLEN;
        attr_left -= static_cast<int>(NLA_ALIGN(attr->nla_len));
        if (attr->nla_type != KAUX_ATTR_STATUS)
          continue;
        if (alen != static_cast<int>(sizeof(kaux_status))) {
          Log::warn("status of {} byte(s), expected {}", alen,
                    sizeof(kaux_status));
          return std::nullopt;
        }
        kaux_status st{};
        std::memcpy(&st, reinterpret_cast<const std::byte *>(attr) + NLA_HDRLEN,
                    sizeof(st));
        if (st.magic != KAUX_STATUS_MAGIC || st.size != sizeof(st) ||
            st.version != KAUX_FAMILY_VERSION) {
          Log::warn("status does not look like ours (magic {:x}, size {})",
                    st.magic, st.size);
          return std::nullopt;
        }
        return st;
      }
      break;
    }
    left -= step;
    cursor += step;
  }
  return std::nullopt;
}

bool NetlinkClient::push_apks(std::span<const ApkEntry> entries) {
  /*
   * The same stream the policy uses: BEGIN says the set is the apks, every
   * record names one file by its numbers, END applies them. The kernel takes
   * the difference this way -- an add for a file that is new or was replaced, a
   * drop for one that is gone -- so the number of apps a device has is not a
   * number this side has to think about.
   */
  std::vector<std::vector<std::byte>> records;
  records.reserve(entries.size());
  for (const auto &entry : entries) {
    /* A drop carries no path: the file is gone, and the module's ledger only
     * has (dev, ino) left for it. */
    const std::string_view path =
        entry.action == 0 ? std::string_view{entry.path} : std::string_view{};
    records.push_back(
        apk_record(entry.action, entry.uid, entry.dev, entry.ino, path));
  }

  for (int attempt = 0; attempt < 2; ++attempt) {
    if (send_stream(KAUX_KIND_APKS, records)) {
      note_reachable();
      return true;
    }
    family_.reset();
    socket_.reset();
  }
  if (!warned_) {
    warned_ = true;
    Log::warn("kernel side unreachable, caller apk table unchanged");
    report_status("kernel unreachable");
  }
  return false;
}

bool NetlinkClient::push_token(std::span<const std::uint32_t, 4> words) {
  std::vector<std::vector<std::byte>> records{token_batch(words.data())};

  for (int attempt = 0; attempt < 2; ++attempt) {
    if (send_stream(KAUX_KIND_TOKEN, records)) {
      note_reachable();
      return true;
    }
    family_.reset();
    socket_.reset();
  }
  return false;
}

} // namespace uidfake
