enum { DIAGNOSTICS_RECORD_CAPACITY = 524288, DIAGNOSTICS_EVENT_CAPACITY = 65536 };
typedef struct DiagnosticsRecord {
    u64 start, end, user, kernel, status;
    u32 kind, graph, window, step, position, layer, phase, sampled;
} DiagnosticsRecord;
typedef struct DiagnosticsEvent {
    u64 status, value;
    u32 record, parent, type, unit;
    char name[160];
} DiagnosticsEvent;
typedef struct DiagnosticsGraph {
    QnnGraphHandle handle;
    u32 calls;
    char name[128];
} DiagnosticsGraph;
static const QnnInterfaceV2 *diagnostics_original_api;
static QnnInterfaceV2 diagnostics_api;
static QnnProfileHandle diagnostics_execution_profile;
static DiagnosticsGraph diagnostics_graphs[128];
static DiagnosticsRecord *diagnostics_records;
static DiagnosticsEvent *diagnostics_event_records;
static u32 diagnostics_graph_count, diagnostics_record_count, diagnostics_event_count;
static u32 diagnostics_dropped, diagnostics_event_dropped, diagnostics_errors;
static u32 diagnostics_window, diagnostics_step, diagnostics_position;
static u32 diagnostics_active[8];
static void *diagnostics_file;
static char diagnostics_output_buffer[65536];
static u32 diagnostics_output_used;
static int diagnostics_io_failed;
static u64 diagnostics_frequency, diagnostics_origin;
static u64 diagnostics_profile_overhead;

__declspec(dllimport) int GetProcessTimes(void *, u64 *, u64 *, u64 *, u64 *);

static void diagnostics_copy(char *target, u32 capacity, const char *source) {
    u32 index = 0U;
    if (source != 0) {
        while (index + 1U < capacity && source[index] != '\0') {
            target[index] = source[index];
            ++index;
        }
    }
    target[index] = '\0';
}

static u64 diagnostics_now(void) {
    long long counter = 0;
    QueryPerformanceCounter(&counter);
    return (u64)counter;
}

static u32 diagnostics_begin(u32 kind, u32 graph, u32 phase, u32 layer) {
    DiagnosticsRecord *record;
    u32 index;
    u64 created, exited;
    if (diagnostics_records == 0) return ~0U;
    if (diagnostics_record_count == DIAGNOSTICS_RECORD_CAPACITY) {
        ++diagnostics_dropped;
        return ~0U;
    }
    index = diagnostics_record_count++;
    record = &diagnostics_records[index];
    record->kind = kind;
    record->graph = graph;
    record->phase = phase;
    record->layer = layer;
    record->window = diagnostics_window;
    record->step = diagnostics_step;
    record->position = diagnostics_position;
    if (kind == 2U && (phase == 0U || phase == 4U)) {
        if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &record->kernel, &record->user)) {
            ++diagnostics_errors;
        }
    }
    record->start = diagnostics_now();
    return index;
}

static void diagnostics_end(u32 index, u64 status) {
    DiagnosticsRecord *record;
    u64 created, exited, kernel = 0U, user = 0U;
    if (index == ~0U) return;
    record = &diagnostics_records[index];
    record->end = diagnostics_now();
    record->status = status;
    if (record->kind == 2U && (record->phase == 0U || record->phase == 4U)) {
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
            record->kernel = kernel - record->kernel;
            record->user = user - record->user;
        } else {
            record->kernel = record->user = 0U;
            ++diagnostics_errors;
        }
    }
}

static void diagnostics_decoder_trace(void *context, u32 phase, u32 begin, u32 position, u32 layer) {
    (void)context;
    if (phase >= 8U) return;
    if (begin) {
        diagnostics_position = position;
        if (phase == 0U) ++diagnostics_step;
        diagnostics_active[phase] = diagnostics_begin(2U, ~0U, phase, layer);
    } else {
        diagnostics_end(diagnostics_active[phase], 0U);
    }
}

static u64 diagnostics_retrieve(QnnContextHandle context, const char *name, QnnGraphHandle *graph) {
    u64 status = diagnostics_original_api->graph_retrieve(context, name, graph);
    if (status == 0U && diagnostics_graph_count < 128U) {
        DiagnosticsGraph *entry = &diagnostics_graphs[diagnostics_graph_count++];
        entry->handle = *graph;
        diagnostics_copy(entry->name, sizeof(entry->name), name);
    } else if (status == 0U) ++diagnostics_errors;
    return status;
}

static void diagnostics_events(const u64 *events, u32 count, u32 depth, u32 record, u32 parent) {
    if (depth > 8U) { ++diagnostics_event_dropped; return; }
    for (u32 index = 0U; index < count; ++index) {
        QnnProfileEventData data = {0};
        const u64 *children = 0;
        u32 child_count = 0U;
        u32 entry_index;
        DiagnosticsEvent *entry;
        if (diagnostics_event_count == DIAGNOSTICS_EVENT_CAPACITY) {
            diagnostics_event_dropped += count - index;
            return;
        }
        entry_index = diagnostics_event_count++;
        entry = &diagnostics_event_records[entry_index];
        entry->record = record;
        entry->parent = parent;
        entry->status = diagnostics_original_api->profile_get_event_data(events[index], &data);
        entry->type = data.type;
        entry->unit = data.unit;
        entry->value = data.value;
        diagnostics_copy(entry->name, sizeof(entry->name), data.identifier);
        if (entry->status != 0U) ++diagnostics_errors;
        if (diagnostics_original_api->profile_get_sub_events(events[index], &children, &child_count) == 0U) {
            diagnostics_events(children, child_count, depth + 1U, record, entry_index);
        } else ++diagnostics_errors;
    }
}

static u64 diagnostics_execute(
    QnnGraphHandle graph, const QnnTensor *inputs, u32 input_count,
    QnnTensor *outputs, u32 output_count, QnnProfileHandle profile, void *signal
) {
    QnnProfileHandle sampled_profile = 0;
    const u64 *events = 0;
    u32 count = 0U, graph_index = ~0U, record;
    u64 status, overhead_start = diagnostics_now();
    for (u32 index = 0U; index < diagnostics_graph_count; ++index) {
        if (diagnostics_graphs[index].handle == graph) { graph_index = index; break; }
    }
    if (graph_index != ~0U && diagnostics_records != 0) {
        u32 calls = ++diagnostics_graphs[graph_index].calls;
        if (diagnostics_level <= 2U && (calls == 1U || calls % 127U == 0U) &&
            diagnostics_event_count < DIAGNOSTICS_EVENT_CAPACITY &&
            diagnostics_original_api->profile_get_events != 0 &&
            diagnostics_original_api->profile_get_sub_events != 0 &&
            diagnostics_original_api->profile_get_event_data != 0) {
            sampled_profile = diagnostics_execution_profile;
        }
    }
    diagnostics_profile_overhead += diagnostics_now() - overhead_start;
    record = diagnostics_begin(1U, graph_index, 0U, ~0U);
    if (record != ~0U) diagnostics_records[record].sampled = sampled_profile != 0;
    status = diagnostics_original_api->graph_execute(
        graph, inputs, input_count, outputs, output_count,
        diagnostics_level == 2U ? diagnostics_execution_profile :
        (sampled_profile ? sampled_profile : profile), signal
    );
    diagnostics_end(record, status);
    if (status != 0U) ++diagnostics_errors;
    overhead_start = diagnostics_now();
    if (sampled_profile != 0) {
        if (diagnostics_original_api->profile_get_events(sampled_profile, &events, &count) == 0U) {
            diagnostics_events(events, count, 0U, record, ~0U);
        } else ++diagnostics_errors;
    }
    diagnostics_profile_overhead += diagnostics_now() - overhead_start;
    return status;
}

static int diagnostics_initialize(const QnnInterfaceV2 *api, QnnBackendHandle backend, u64 frequency, u64 origin) {
    diagnostics_file = CreateFileA(diagnostics_path, 0x40000000U, 1U, 0, 2U, 0x80U, 0);
    if (diagnostics_file == (void *)(usize)-1) { diagnostics_file = 0; return 0; }
    diagnostics_records = VirtualAlloc(0, sizeof(DiagnosticsRecord) * DIAGNOSTICS_RECORD_CAPACITY, 0x3000U, 4U);
    diagnostics_event_records = VirtualAlloc(0, sizeof(DiagnosticsEvent) * DIAGNOSTICS_EVENT_CAPACITY, 0x3000U, 4U);
    if (diagnostics_records == 0 || diagnostics_event_records == 0) return 0;
    diagnostics_original_api = api;
    diagnostics_frequency = frequency;
    diagnostics_origin = origin;
    diagnostics_api = *api;
    diagnostics_api.graph_execute = diagnostics_execute;
    diagnostics_api.graph_retrieve = diagnostics_retrieve;
    if (diagnostics_level <= 2U && api->profile_create(backend, diagnostics_level, &diagnostics_execution_profile) != 0U) {
        ++diagnostics_errors;
    }
    if (diagnostics_level <= 2U && (api->profile_get_events == 0 ||
        api->profile_get_sub_events == 0 || api->profile_get_event_data == 0)) ++diagnostics_errors;
    return 1;
}

static void diagnostics_release_profile(void) {
    if (diagnostics_execution_profile != 0) {
        if (diagnostics_original_api->profile_free(diagnostics_execution_profile) != 0U) ++diagnostics_errors;
        diagnostics_execution_profile = 0;
    }
}

static void diagnostics_flush(void) {
    u32 offset = 0U;
    while (offset < diagnostics_output_used && !diagnostics_io_failed) {
        u32 written = 0U;
        if (!WriteFile(diagnostics_file, diagnostics_output_buffer + offset,
                diagnostics_output_used - offset, &written, 0) || written == 0U) {
            diagnostics_io_failed = 1;
            break;
        }
        offset += written;
    }
    diagnostics_output_used = 0U;
}

static void diagnostics_char(char value) {
    if (diagnostics_output_used == sizeof(diagnostics_output_buffer)) diagnostics_flush();
    diagnostics_output_buffer[diagnostics_output_used++] = value;
}

static void diagnostics_text(const char *text) {
    while (*text) diagnostics_char(*text++);
}

static void diagnostics_number(u64 value) {
    char digits[20];
    u32 count = 0U;
    do { digits[count++] = (char)('0' + value % 10U); value /= 10U; } while (value);
    while (count) diagnostics_char(digits[--count]);
}

static void diagnostics_field(u64 value) {
    diagnostics_char(','); diagnostics_number(value);
}

static void diagnostics_name(const char *name) {
    diagnostics_text(",\"");
    while (*name) {
        if (*name == '"') diagnostics_char('"');
        diagnostics_char(*name++);
    }
    diagnostics_text("\"\n");
}

static void diagnostics_finish(void) {
    if (diagnostics_file != 0) {
        diagnostics_text("kind,id,graph,window,step,position,layer,phase,start,end,user_100ns,kernel_100ns,status,sampled,name\n");
        diagnostics_text("meta");
        diagnostics_field(1U); diagnostics_field(diagnostics_frequency);
        diagnostics_field(diagnostics_origin); diagnostics_field(diagnostics_level);
        diagnostics_field(diagnostics_dropped); diagnostics_field(diagnostics_event_dropped);
        diagnostics_field(diagnostics_errors); diagnostics_field(diagnostics_profile_overhead);
        diagnostics_field(diagnostics_record_count); diagnostics_field(diagnostics_event_count);
        diagnostics_field(127U); diagnostics_field(active_model->model_id);
        diagnostics_field(decoder_offload_mask); diagnostics_name("schema1");
        for (u32 index = 0U; index < diagnostics_record_count; ++index) {
            DiagnosticsRecord *record = &diagnostics_records[index];
            diagnostics_text(record->kind == 1U ? "graph" : "phase");
            diagnostics_field(index); diagnostics_field(record->graph);
            diagnostics_field(record->window); diagnostics_field(record->step);
            diagnostics_field(record->position); diagnostics_field(record->layer);
            diagnostics_field(record->phase); diagnostics_field(record->start);
            diagnostics_field(record->end); diagnostics_field(record->user);
            diagnostics_field(record->kernel); diagnostics_field(record->status);
            diagnostics_field(record->sampled);
            diagnostics_name(record->graph < diagnostics_graph_count ? diagnostics_graphs[record->graph].name : "");
        }
        for (u32 index = 0U; index < diagnostics_event_count; ++index) {
            DiagnosticsEvent *event = &diagnostics_event_records[index];
            diagnostics_text("event"); diagnostics_field(index);
            diagnostics_field(event->record); diagnostics_field(event->parent);
            diagnostics_field(event->type); diagnostics_field(event->unit);
            diagnostics_field(event->value);
            for (u32 padding = 0U; padding < 5U; ++padding) diagnostics_field(0U);
            diagnostics_field(event->status); diagnostics_field(0U); diagnostics_name(event->name);
        }
        diagnostics_flush();
        if (!CloseHandle(diagnostics_file)) diagnostics_io_failed = 1;
        diagnostics_file = 0;
    }
    if (diagnostics_records != 0) VirtualFree(diagnostics_records, 0U, 0x8000U);
    if (diagnostics_event_records != 0) VirtualFree(diagnostics_event_records, 0U, 0x8000U);
    diagnostics_records = 0;
    diagnostics_event_records = 0;
}