// SPDX-License-Identifier: GPL-2.0
// Host test for the wire side of an upload: the bytes of a stream have to be
// exactly what src/netlink.c reads, and a batch has to fit one netlink message.
// The values below are the ones scripts/test-netlink-flow.py drives the module's
// own handlers with, so the two ends are pinned against each other.
#include "paging.hpp"

#include <cstdio>
#include <cstdlib>

using uidfake::Pair;

namespace
{

void must(bool ok, const char *what)
{
	if (ok)
		return;
	std::fprintf(stderr, "paging: FAILED (%s)\n", what);
	std::exit(1);
}

/* One little-endian word of a payload, the way the kernel reads it. */
std::uint32_t word(const std::vector<std::byte> &bytes, std::size_t at)
{
	std::uint32_t out = 0;

	for (std::size_t i = 0; i < 4; i++) {
		const std::uint8_t byte = static_cast<std::uint8_t>(bytes[at + i]);

		out |= static_cast<std::uint32_t>(byte) << (8 * i);
	}
	return out;
}

} // namespace

int main()
{
	/* BEGIN: struct kaux_begin, the kind alone. */
	const auto begin = uidfake::begin_payload(KAUX_KIND_POLICY);

	must(begin.size() == 4, "begin is one word");
	must(word(begin, 0) == 0, "policy is kind 0");

	/* One policy record: op, caller, target, flags. */
	const auto record = uidfake::policy_record(10123, 64820);

	must(record.size() == uidfake::kPolicyRecWords * 4,
	     "a policy record is four words");
	must(word(record, 0) == KAUX_OP_ADD, "a record is an add");
	must(word(record, 4) == 10123, "the caller is the second word");
	must(word(record, 8) == 64820, "the target is the third");
	must(word(record, 12) == 0, "the module reads no flags");

	/* One apk record, with its path padded to a four byte boundary: the head
	 * the python flow test drives the handler with is the same shape. */
	const auto apk = uidfake::apk_record(KAUX_OP_ADD, 10123, 64820,
					     0x12345678abcdef01ULL, "apk");
	must(apk.size() == uidfake::kApkRecWords * 4 + 4,	     "an apk record is six words and its path");
	must(word(apk, 0) == KAUX_OP_ADD, "an apk record is an add");
	must(word(apk, 4) == 10123, "the uid is the second word");
	must(word(apk, 8) == 64820, "the device is the third");
	must(word(apk, 12) == 0xabcdef01u,
	     "the inode's low half is the fourth");
	must(word(apk, 16) == 0x12345678u, "its high half is the fifth");
	must(word(apk, 20) == 4, "the path length is the sixth");
	must(word(apk, 24) == 0x006b7061u, "and the path follows, zero padded");

	/* A drop carries no path: the file is gone, and the numbers name it. */
	const auto drop = uidfake::apk_record(KAUX_OP_DEL, 0, 64820,
					      0x12345678abcdef01ULL, {});

	must(drop.size() == uidfake::kApkRecWords * 4,
	     "a drop is the head alone");
	must(word(drop, 0) == KAUX_OP_DEL, "a drop is a drop");

	/* The token stream's one batch: the count, then the four words. */
	constexpr std::uint32_t kToken[4] = { 0x03020100u, 0x87868584u,
					      0x0b0a0908u, 0xfffefdfcu };
	const auto token = uidfake::token_batch(kToken);

	must(token.size() == 4 + uidfake::kTokenWords * 4,
	     "a token batch is the count and four words");
	must(word(token, 0) == uidfake::kTokenWords, "the count comes first");
	must(word(token, 4) == kToken[0], "then the name, word by word");
	must(word(token, 16) == kToken[3], "the last word is the fourth");

	/* A batch is split so that no message goes over the module's cap. */
	must(4 + uidfake::kPolicyRecWords * 4 * 2047 <= uidfake::kMessageBytes,
	     "2047 policy records fit one message");
	must(4 + uidfake::kPolicyRecWords * 4 * 2048 > uidfake::kMessageBytes,
	     "and 2048 do not: that is where a batch is split");
	must(4 + uidfake::kApkRecWords * 4 + 4 <= uidfake::kMessageBytes,
	     "one apk record fits one message");

	/* st_dev travels as stat(2) reports it: the module compares against
	 * cp_new_stat()'s encoding, which is the same layout. */
	must(uidfake::encode_dev(0x08000101u) == 0x08000101u,
	     "the device number travels as it stands");

	std::printf("paging: PASS\n");
	return 0;
}
