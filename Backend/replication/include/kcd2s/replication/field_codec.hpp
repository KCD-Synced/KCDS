// Per-field wire codec: encode/decode one schema field to/from the bit
// stream, honoring its kind + quantization. Shared by the snapshot codec (downstream) and the
// upstream owner-input codec, so both sides agree on exactly how a field is packed.
#ifndef KCD2S_REPLICATION_FIELD_CODEC_HPP
#define KCD2S_REPLICATION_FIELD_CODEC_HPP

#include "kcd2s/core/bits.hpp"
#include "kcd2s/replication/schema.hpp"
#include "kcd2s/replication/world.hpp"

namespace kcd2s::replication {

void encode_field(core::BitWriter& w, const FieldDef& fd, const FieldValue& v);
FieldValue decode_field(core::BitReader& r, const FieldDef& fd);

// True when encode_field() emits exactly the same bits for both values under `fd`, i.e. a client
// that holds `a` cannot observe any difference after receiving `b`. Exact kinds (raw floats,
// integers, booleans, strings) compare directly; raw floats compare bit patterns, so NaN payloads
// and signed zeros count as different. Quantized floats and quaternions compare their quantized
// encodings, so a sub-step change a client could never observe is not reported as a change.
bool same_encoding(const FieldDef& fd, const FieldValue& a, const FieldValue& b);

}  // namespace kcd2s::replication

#endif  // KCD2S_REPLICATION_FIELD_CODEC_HPP
