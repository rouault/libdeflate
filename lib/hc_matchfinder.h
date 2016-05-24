/*
 * hc_matchfinder.h - Lempel-Ziv matchfinding with a hash table of linked lists
 *
 * Written in 2014-2016 by Eric Biggers <ebiggers3@gmail.com>
 *
 * To the extent possible under law, the author(s) have dedicated all copyright
 * and related and neighboring rights to this software to the public domain
 * worldwide. This software is distributed without any warranty.
 *
 * You should have received a copy of the CC0 Public Domain Dedication along
 * with this software. If not, see
 * <http://creativecommons.org/publicdomain/zero/1.0/>.
 *
 * ---------------------------------------------------------------------------
 *
 *				   Algorithm
 *
 * This is a Hash Chains (hc) based matchfinder.
 *
 * The main data structure is a hash table where each hash bucket contains a
 * linked list (or "chain") of sequences whose first 4 bytes share the same hash
 * code.  Each sequence is identified by its starting position in the input
 * buffer.
 *
 * The algorithm processes the input buffer sequentially.  At each byte
 * position, the hash code of the first 4 bytes of the sequence beginning at
 * that position (the sequence being matched against) is computed.  This
 * identifies the hash bucket to use for that position.  Then, this hash
 * bucket's linked list is searched for matches.  Then, a new linked list node
 * is created to represent the current sequence and is prepended to the list.
 *
 * This algorithm has several useful properties:
 *
 * - It only finds true Lempel-Ziv matches; i.e., those where the matching
 *   sequence occurs prior to the sequence being matched against.
 *
 * - The sequences in each linked list are always sorted by decreasing starting
 *   position.  Therefore, the closest (smallest offset) matches are found
 *   first, which in many compression formats tend to be the cheapest to encode.
 *
 * - Although fast running time is not guaranteed due to the possibility of the
 *   lists getting very long, the worst degenerate behavior can be easily
 *   prevented by capping the number of nodes searched at each position.
 *
 * - If the compressor decides not to search for matches at a certain position,
 *   then that position can be quickly inserted without searching the list.
 *
 * - The algorithm is adaptable to sliding windows: just store the positions
 *   relative to a "base" value that is updated from time to time, and stop
 *   searching each list when the sequences get too far away.
 *
 * ----------------------------------------------------------------------------
 *
 *				 Optimizations
 *
 * The main hash table and chains handle length 4+ matches only.
 *
 * The longest_match() and skip_positions() functions are inlined into the
 * compressors that use them.  This isn't just about saving the overhead of a
 * function call.  These functions are intended to be called from the inner
 * loops of compressors, where giving the compiler more control over register
 * allocation is very helpful.  There is also significant benefit to be gained
 * from allowing the CPU to predict branches independently at each call site.
 * For example, "lazy"-style compressors can be written with two calls to
 * longest_match(), each of which starts with a different 'best_len' and
 * therefore has significantly different performance characteristics.
 *
 * Although any hash function can be used, a multiplicative hash is fast and
 * works well.
 *
 * On some processors, it is significantly faster to extend matches by whole
 * words (32 or 64 bits) instead of by individual bytes.  For this to be the
 * case, the processor must implement unaligned memory accesses efficiently and
 * must have either a fast "find first set bit" instruction or a fast "find last
 * set bit" instruction, depending on the processor's endianness.
 *
 * The code uses one loop for finding the first match and one loop for finding a
 * longer match.  Each of these loops is tuned for its respective task and in
 * combination are faster than a single generalized loop that handles both
 * tasks.
 *
 * The code also uses a tight inner loop that only compares the last and first
 * bytes of a potential match.  It is only when these bytes match that a full
 * match extension is attempted.
 *
 * ----------------------------------------------------------------------------
 */

#include "matchfinder_common.h"

#include <emmintrin.h>
#include <smmintrin.h>

#define HC_MATCHFINDER_HASH4_ORDER	16
#define HC_MATCHFINDER_HASH16_ORDER	16

#define HC_MATCHFINDER_TOTAL_HASH_LENGTH	\
	(1UL << HC_MATCHFINDER_HASH4_ORDER) +	\
	(1UL << HC_MATCHFINDER_HASH16_ORDER)

struct hc_matchfinder {

	/* The hash table which contains the first nodes of the linked lists for
	 * finding length 4+ matches  */
	mf_pos_t hash4_tab[1UL << HC_MATCHFINDER_HASH4_ORDER];

	mf_pos_t hash16_tab[1UL << HC_MATCHFINDER_HASH16_ORDER];

	/* The "next node" references for the linked lists.  The "next node" of
	 * the node for the sequence with position 'pos' is 'next_tab[pos]'.  */
	mf_pos_t next_tab[MATCHFINDER_WINDOW_SIZE];

	mf_pos_t next16_tab[MATCHFINDER_WINDOW_SIZE];

}
#ifdef _aligned_attribute
  _aligned_attribute(MATCHFINDER_ALIGNMENT)
#endif
;

/* Prepare the matchfinder for a new input buffer.  */
static forceinline void
hc_matchfinder_init(struct hc_matchfinder *mf)
{
	matchfinder_init((mf_pos_t *)mf, HC_MATCHFINDER_TOTAL_HASH_LENGTH);
}

static forceinline void
hc_matchfinder_slide_window(struct hc_matchfinder *mf)
{
	matchfinder_rebase((mf_pos_t *)mf,
			   sizeof(struct hc_matchfinder) / sizeof(mf_pos_t));
}

#define HASH16_MULTIPLIER	31
#define HASH16_POP_FACTOR	1353309697	/* 31**15 % (1<<32) */
static u8 hash16_history[16];
static u32 hash16_hindex;

static forceinline u32 hash16_push(u32 sum, u8 byte)
{
	sum -= hash16_history[hash16_hindex] * HASH16_POP_FACTOR;
	sum += byte;
	sum *= HASH16_MULTIPLIER;
	hash16_history[hash16_hindex++] = byte;
	hash16_hindex %= ARRAY_LEN(hash16_history);
	return sum;
}

/*
 * Find the longest match longer than 'best_len' bytes.
 *
 * @mf
 *	The matchfinder structure.
 * @in_base_p
 *	Location of a pointer which points to the place in the input data the
 *	matchfinder currently stores positions relative to.  This may be updated
 *	by this function.
 * @cur_pos
 *	The current position in the input buffer relative to @in_base (the
 *	position of the sequence being matched against).
 * @best_len
 *	Require a match longer than this length.
 * @max_len
 *	The maximum permissible match length at this position.
 * @nice_len
 *	Stop searching if a match of at least this length is found.
 *	Must be <= @max_len.
 * @max_search_depth
 *	Limit on the number of potential matches to consider.  Must be >= 1.
 * @next_hashes
 *	The precomputed hash codes for the sequence beginning at @in_next.
 *	These will be used and then updated with the precomputed hashcodes for
 *	the sequence beginning at @in_next + 1.
 * @offset_ret
 *	If a match is found, its offset is returned in this location.
 *
 * Return the length of the match found, or 'best_len' if no match longer than
 * 'best_len' was found.
 */
static forceinline u32
hc_matchfinder_longest_match(struct hc_matchfinder * const restrict mf,
			     const u8 ** const restrict in_base_p,
			     const u8 * const restrict in_next,
			     u32 best_len,
			     const u32 max_len,
			     const u32 nice_len,
			     const u32 max_search_depth,
			     u32 * const restrict next_hashes,
			     u32 * const restrict offset_ret)
{
	u32 depth_remaining = max_search_depth;
	const u8 *best_matchptr = in_next;
	mf_pos_t cur_node4, cur_node16;
	u32 hash4;
	u32 hash16;
	u32 next_seq4;
	u32 seq4;
	const u8 *matchptr;
	u32 len;
	u32 cur_pos = in_next - *in_base_p;
	const u8 *in_base;
	mf_pos_t cutoff;

	if (cur_pos == MATCHFINDER_WINDOW_SIZE) {
		hc_matchfinder_slide_window(mf);
		*in_base_p += MATCHFINDER_WINDOW_SIZE;
		cur_pos = 0;
	}

	in_base = *in_base_p;
	cutoff = cur_pos - MATCHFINDER_WINDOW_SIZE;

	if (unlikely(max_len < 17))
		goto out;

	hash4 = next_hashes[0];
	hash16 = next_hashes[1];

	cur_node4 = mf->hash4_tab[hash4];
	cur_node16 = mf->hash16_tab[hash16 % ARRAY_LEN(mf->hash16_tab)];

	mf->hash4_tab[hash4] = cur_pos;
	mf->next_tab[cur_pos] = cur_node4;

	mf->hash16_tab[hash16 % ARRAY_LEN(mf->hash16_tab)] = cur_pos;
	mf->next16_tab[cur_pos] = cur_node16;

	next_seq4 = load_u32_unaligned(in_next + 1);
	next_hashes[0] = lz_hash(next_seq4, HC_MATCHFINDER_HASH4_ORDER);
	next_hashes[1] = hash16_push(next_hashes[1], *(in_next + 16));
	prefetchw(&mf->hash4_tab[next_hashes[0]]);
	prefetchw(&mf->hash16_tab[next_hashes[1] % ARRAY_LEN(mf->hash16_tab)]);

	if (cur_node16 > cutoff) {
		__m128i seq16 = _mm_loadu_si128((__m128i *)in_next);
		for (;;) {
			__m128i match16;
			__m128i neq;

			matchptr = &in_base[cur_node16];
			match16 = _mm_loadu_si128((__m128i *)matchptr);

			neq = _mm_xor_si128(match16, seq16);
			if (_mm_test_all_zeros(neq, neq)) {
				len = lz_extend(in_next, matchptr, 16, max_len);
				if (len > best_len) {
					/* This is the new longest match.  */
					best_len = len;
					best_matchptr = matchptr;
					if (best_len >= nice_len)
						goto out;
				}
			}

			cur_node16 = mf->next16_tab[cur_node16 & (MATCHFINDER_WINDOW_SIZE - 1)];
			if (cur_node16 <= cutoff || !--depth_remaining)
				break;
		}
	}

	if (best_len < 4) {  /* No match of length >= 4 found yet?  */

		/* Check for a length 4 match.  */

		if (cur_node4 <= cutoff)
			goto out;

		seq4 = load_u32_unaligned(in_next);

		for (;;) {
			/* No length 4 match found yet.  Check the first 4 bytes.  */
			matchptr = &in_base[cur_node4];

			if (load_u32_unaligned(matchptr) == seq4)
				break;

			/* The first 4 bytes did not match.  Keep trying.  */
			cur_node4 = mf->next_tab[cur_node4 & (MATCHFINDER_WINDOW_SIZE - 1)];
			if (cur_node4 <= cutoff || !--depth_remaining)
				goto out;
		}

		/* Found a match of length >= 4.  Extend it to its full length.  */
		best_matchptr = matchptr;
		best_len = lz_extend(in_next, best_matchptr, 4, max_len);
		if (best_len >= nice_len)
			goto out;
		cur_node4 = mf->next_tab[cur_node4 & (MATCHFINDER_WINDOW_SIZE - 1)];
		if (cur_node4 <= cutoff || !--depth_remaining)
			goto out;
	} else {
		if (cur_node4 <= cutoff || best_len >= nice_len)
			goto out;
	}

	/* Check for matches of length >= 5.  */

	for (;;) {
		for (;;) {
			matchptr = &in_base[cur_node4];

			/* Already found a length 4 match.  Try for a longer
			 * match; start by checking either the last 4 bytes and
			 * the first 4 bytes, or the last byte.  (The last byte,
			 * the one which would extend the match length by 1, is
			 * the most important.)  */
		#if UNALIGNED_ACCESS_IS_FAST
			if ((load_u32_unaligned(matchptr + best_len - 3) ==
			     load_u32_unaligned(in_next + best_len - 3)) &&
			    (load_u32_unaligned(matchptr) ==
			     load_u32_unaligned(in_next)))
		#else
			if (matchptr[best_len] == in_next[best_len])
		#endif
				break;

			/* Continue to the next node in the list.  */
			cur_node4 = mf->next_tab[cur_node4 & (MATCHFINDER_WINDOW_SIZE - 1)];
			if (cur_node4 <= cutoff || !--depth_remaining)
				goto out;
		}

	#if UNALIGNED_ACCESS_IS_FAST
		len = 4;
	#else
		len = 0;
	#endif
		len = lz_extend(in_next, matchptr, len, max_len);
		if (len > best_len) {
			/* This is the new longest match.  */
			best_len = len;
			best_matchptr = matchptr;
			if (best_len >= nice_len)
				goto out;
		}

		/* Continue to the next node in the list.  */
		cur_node4 = mf->next_tab[cur_node4 & (MATCHFINDER_WINDOW_SIZE - 1)];
		if (cur_node4 <= cutoff || !--depth_remaining)
			goto out;
	}
out:
	*offset_ret = in_next - best_matchptr;
	return best_len;
}

/*
 * Advance the matchfinder, but don't search for matches.
 *
 * @mf
 *	The matchfinder structure.
 * @in_base_p
 *	Location of a pointer which points to the place in the input data the
 *	matchfinder currently stores positions relative to.  This may be updated
 *	by this function.
 * @cur_pos
 *	The current position in the input buffer relative to @in_base.
 * @end_pos
 *	The end position of the input buffer, relative to @in_base.
 * @next_hashes
 *	The precomputed hash codes for the sequence beginning at @in_next.
 *	These will be used and then updated with the precomputed hashcodes for
 *	the sequence beginning at @in_next + @count.
 * @count
 *	The number of bytes to advance.  Must be > 0.
 *
 * Returns @in_next + @count.
 */
static forceinline const u8 *
hc_matchfinder_skip_positions(struct hc_matchfinder * const restrict mf,
			      const u8 ** const restrict in_base_p,
			      const u8 *in_next,
			      const u8 * const in_end,
			      const u32 count,
			      u32 * const restrict next_hashes)
{
	u32 cur_pos;
	u32 hash4;
	u16 hash16;
	u32 next_seq4;
	u32 remaining = count;

	if (unlikely(count + 17 > in_end - in_next))
		return &in_next[count];

	cur_pos = in_next - *in_base_p;
	hash4 = next_hashes[0];
	hash16 = next_hashes[1];
	do {
		if (cur_pos == MATCHFINDER_WINDOW_SIZE) {
			hc_matchfinder_slide_window(mf);
			*in_base_p += MATCHFINDER_WINDOW_SIZE;
			cur_pos = 0;
		}
		mf->next_tab[cur_pos] = mf->hash4_tab[hash4];
		mf->hash4_tab[hash4] = cur_pos;

		mf->next16_tab[cur_pos] = mf->hash16_tab[hash16 % ARRAY_LEN(mf->hash16_tab)];
		mf->hash16_tab[hash16 % ARRAY_LEN(mf->hash16_tab)] = cur_pos;

		next_seq4 = load_u32_unaligned(in_next + 1);
		hash4 = lz_hash(next_seq4, HC_MATCHFINDER_HASH4_ORDER);
		hash16 = hash16_push(hash16, *(in_next + 16));
		in_next++;
		cur_pos++;
	} while (--remaining);

	prefetchw(&mf->hash4_tab[hash4]);
	prefetchw(&mf->hash16_tab[hash16 % ARRAY_LEN(mf->hash16_tab)]);
	next_hashes[0] = hash4;
	next_hashes[1] = hash16;

	return in_next;
}
