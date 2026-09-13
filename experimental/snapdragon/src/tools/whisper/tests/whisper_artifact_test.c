#include "whisper_artifact.h"

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

__declspec(dllimport) void ExitProcess(u32 status);

static void set_header(
    WhisperArtifactHeader *header,
    const WhisperModelConfig *model,
    u32 payload_type,
    u32 element_type,
    const void *payload,
    u64 payload_size
) {
    header->model_id = model->model_id;
    header->payload_type = payload_type;
    header->element_type = element_type;
    header->element_count = payload_size;
    header->payload_size = payload_size;
    header->payload_hash = whisper_artifact_hash_update(
        WHISPER_ARTIFACT_HASH_OFFSET_BASIS, payload, payload_size
    );
    header->width = model->width;
    header->ffn_width = model->ffn_width;
    header->attention_heads = model->attention_heads;
    header->encoder_layers = model->encoder_layers;
    header->decoder_layers = model->decoder_layers;
    header->vocabulary_size = model->vocabulary_size;
    header->text_context = model->text_context;
    header->mel_bins = model->mel_bins;
    header->encoder_frames = model->encoder_frames;
}

void mainCRTStartup(void) {
    static const u8 payload[] = {0x76U, 0x32U, 0x2dU, 0x74U, 0x65U, 0x73U, 0x74U};
    static const WhisperModelConfig base = {
        2U, "base", 512U, 2048U, 8U, 6U, 6U, 51865U, 448U, 80U, 1500U
    };
    const WhisperModelConfig *tiny = whisper_model_tiny();
    WhisperArtifactHeader header;
    WhisperArtifactHeader decoded;
    u8 encoded[WHISPER_ARTIFACT_HEADER_SIZE];
    u8 changed_payload[sizeof(payload)];
    u64 size_result;
    u32 index;

    set_header(
        &header, tiny, WHISPER_ARTIFACT_PAYLOAD_TOKEN_BYTES,
        WHISPER_ARTIFACT_ELEMENT_U8, payload, sizeof(payload)
    );
    whisper_artifact_encode_header(encoded, &header);
    if (!whisper_artifact_decode_header(encoded, &decoded)) ExitProcess(1U);
    if (!whisper_artifact_header_valid(
            &decoded, tiny, WHISPER_ARTIFACT_PAYLOAD_TOKEN_BYTES,
            WHISPER_ARTIFACT_ELEMENT_U8, sizeof(payload), sizeof(payload)
        )) ExitProcess(2U);
    if (!whisper_artifact_payload_valid(&decoded, payload, sizeof(payload))) {
        ExitProcess(3U);
    }

    encoded[0] ^= 1U;
    if (whisper_artifact_decode_header(encoded, &decoded)) ExitProcess(4U);
    whisper_artifact_encode_header(encoded, &header);
    encoded[8] = 3U;
    if (whisper_artifact_decode_header(encoded, &decoded)) ExitProcess(5U);
    whisper_artifact_encode_header(encoded, &header);
    if (!whisper_artifact_decode_header(encoded, &decoded)) ExitProcess(6U);

    decoded.model_id += 1U;
    if (whisper_artifact_header_valid(
            &decoded, tiny, WHISPER_ARTIFACT_PAYLOAD_TOKEN_BYTES,
            WHISPER_ARTIFACT_ELEMENT_U8, sizeof(payload), sizeof(payload)
        )) ExitProcess(7U);
    if (!whisper_artifact_decode_header(encoded, &decoded)) ExitProcess(8U);
    decoded.width += 1U;
    if (whisper_artifact_header_valid(
            &decoded, tiny, WHISPER_ARTIFACT_PAYLOAD_TOKEN_BYTES,
            WHISPER_ARTIFACT_ELEMENT_U8, sizeof(payload), sizeof(payload)
        )) ExitProcess(9U);
    if (whisper_artifact_payload_valid(&header, payload, sizeof(payload) - 1U)) {
        ExitProcess(10U);
    }
    for (index = 0U; index < sizeof(payload); ++index) {
        changed_payload[index] = payload[index];
    }
    changed_payload[sizeof(payload) - 1U] ^= 1U;
    if (whisper_artifact_payload_valid(&header, changed_payload, sizeof(payload))) {
        ExitProcess(11U);
    }

    if (!whisper_artifact_decode_header(encoded, &decoded)) ExitProcess(12U);
    decoded.element_count = ~0ULL;
    if (whisper_artifact_header_valid(
            &decoded, tiny, WHISPER_ARTIFACT_PAYLOAD_TOKEN_BYTES,
            WHISPER_ARTIFACT_ELEMENT_U8, sizeof(payload), sizeof(payload)
        )) ExitProcess(13U);
    if (!whisper_artifact_decode_header(encoded, &decoded)) ExitProcess(14U);
    decoded.payload_size = ~0ULL;
    if (whisper_artifact_header_valid(
            &decoded, tiny, WHISPER_ARTIFACT_PAYLOAD_TOKEN_BYTES,
            WHISPER_ARTIFACT_ELEMENT_U8, sizeof(payload), sizeof(payload)
        )) ExitProcess(15U);
    if (whisper_model_size_add(~0ULL, 1U, &size_result) ||
        whisper_model_size_multiply(~0ULL, 2U, &size_result)) {
        ExitProcess(16U);
    }

    set_header(
        &header, tiny, WHISPER_ARTIFACT_PAYLOAD_QNN_CONTEXT,
        WHISPER_ARTIFACT_ELEMENT_BLOB, payload, sizeof(payload)
    );
    if (whisper_artifact_header_valid(
            &header, &base, WHISPER_ARTIFACT_PAYLOAD_QNN_CONTEXT,
            WHISPER_ARTIFACT_ELEMENT_BLOB, sizeof(payload), sizeof(payload)
        )) ExitProcess(17U);
    whisper_artifact_encode_header(encoded, &header);
    encoded[92] = 1U;
    if (whisper_artifact_decode_header(encoded, &decoded)) ExitProcess(18U);

    ExitProcess(0U);
}