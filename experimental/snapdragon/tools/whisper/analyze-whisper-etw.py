import argparse
import collections
import csv
import json
import pathlib
import re
import subprocess


def analyze(lines, pid, image_base, image_size):
	samples = collections.Counter()
	modules = collections.Counter()
	scheduled = collections.Counter()
	states = collections.Counter()
	running = {}
	ready = {}
	ready_delays = []
	thread_ready = collections.defaultdict(list)
	sample_weights = {}
	sample_stacks = collections.defaultdict(list)
	mismatches = 0
	marker = re.compile(r"\(\s*" + str(pid) + r"\)$")
	for line in lines:
		if not line.lstrip().startswith(("SampledProfile,", "CSwitch,", "ReadyThread,", "Stack,")):
			continue
		row = [value.strip() for value in next(csv.reader([line], skipinitialspace=True))]
		if not row[1].isdigit():
			continue
		timestamp = int(row[1])
		if row[0] == "SampledProfile" and marker.search(row[2]):
			address = int(row[4], 16)
			count = int(row[8])
			sample_weights[(timestamp, int(row[3]))] = count
			modules[row[7].split("!", 1)[0]] += count
			if image_base <= address < image_base + image_size:
				samples[address - image_base] += count
		elif row[0] == "Stack":
			key = (timestamp, int(row[2]))
			if key in sample_weights:
				address = int(row[4], 16)
				if image_base <= address < image_base + image_size:
					sample_stacks[key].append(address - image_base - (4 if int(row[3]) > 1 else 0))
		elif row[0] == "ReadyThread" and marker.search(row[4]):
			ready.setdefault(int(row[5]), timestamp)
		elif row[0] == "CSwitch":
			cpu = int(row[16])
			new_thread, old_thread = int(row[3]), int(row[9])
			previous = running.get(cpu)
			if previous:
				start, thread, is_target = previous
				if thread != old_thread:
					mismatches += 1
				elif is_target:
					scheduled[thread] += timestamp - start
			target_new = bool(marker.search(row[2]))
			if target_new:
				ready_time = ready.pop(new_thread, None)
				if ready_time is not None:
					ready_delays.append(timestamp - ready_time)
					thread_ready[new_thread].append(timestamp - ready_time)
			if marker.search(row[8]):
				states[(row[12], row[13])] += 1
				if row[12] in ("Ready", "DeferredReady"):
					ready.setdefault(old_thread, timestamp)
			running[cpu] = (timestamp, new_thread, target_new)
	return samples, modules, scheduled, states, ready_delays, mismatches, sample_weights, sample_stacks, thread_ready


def self_test():
	rows = [
		"CSwitch,100,app (42),7,0,0,0,0,Idle (0),0,0,0,Waiting,Executive,0,0,0",
		"SampledProfile,120,app (42),7,0x1010,0,app!x,app!x,1,Unbatched",
		"Stack,120,7,1,0x1010,app!x",
		"Stack,120,7,2,0x1024,app!caller",
		"Stack,121,7,1,0x1030,app!unrelated",
		"CSwitch,150,Idle (0),0,0,0,0,0,app (42),7,0,0,Ready,Executive,0,0,0",
		"CSwitch,175,app (42),7,0,0,0,0,Idle (0),0,0,0,Waiting,Executive,0,0,0",
		"CSwitch,200,Idle (0),0,0,0,0,0,app (42),7,0,0,Waiting,Executive,0,0,0",
	]
	samples, modules, scheduled, states, delays, mismatches, weights, stacks, thread_ready = analyze(rows, 42, 0x1000, 0x100)
	assert samples == {16: 1} and modules == {"app": 1}
	assert scheduled == {7: 75} and delays == [25] and mismatches == 0
	assert sum(states.values()) == 2
	assert stacks == {(120, 7): [16, 32]} and weights == {(120, 7): 1}
	assert thread_ready == {7: [25]}
	print("PASS sample attribution, context switches and ready delays")


def main():
	parser = argparse.ArgumentParser(description="Stream an Xperf dumper CSV and resolve application samples using LLVM/PDB.")
	parser.add_argument("csv", nargs="?", type=pathlib.Path)
	parser.add_argument("--pid", type=int)
	parser.add_argument("--image", type=pathlib.Path)
	parser.add_argument("--base", type=lambda value: int(value, 0))
	parser.add_argument("--size", type=lambda value: int(value, 0))
	parser.add_argument("--symbolizer", default="llvm-symbolizer")
	parser.add_argument("--output", type=pathlib.Path)
	parser.add_argument("--self-test", action="store_true")
	args = parser.parse_args()
	if args.self_test:
		self_test()
		return
	if any(value is None for value in (args.csv, args.pid, args.image, args.base, args.size, args.output)):
		parser.error("csv, --pid, --image, --base, --size and --output are required")
	with args.csv.open(encoding="utf-8-sig", errors="replace") as source:
		samples, modules, scheduled, states, delays, mismatches, weights, stacks, thread_ready = analyze(source, args.pid, args.base, args.size)
	addresses = sorted(set(samples).union(address for stack in stacks.values() for address in stack))
	if not addresses or not scheduled:
		raise ValueError("No application samples or scheduling intervals found")
	result = subprocess.run(
		[args.symbolizer, "--obj=" + str(args.image), "--relative-address", "--output-style=JSON"],
		input="".join(hex(address) + "\n" for address in addresses),
		text=True, capture_output=True, check=True,
	)
	resolved = []
	for line in result.stdout.splitlines():
		item = json.loads(line)
		resolved.extend(item if isinstance(item, list) else [item])
	if len(resolved) != len(addresses):
		raise ValueError("Incomplete symbolizer output")
	functions = collections.Counter()
	locations = collections.Counter()
	address_functions = {}
	for address, item in zip(addresses, resolved):
		if int(item["Address"], 16) != address:
			raise ValueError("Symbolizer address mismatch")
		frame = item["Symbol"][0]
		address_functions[address] = {entry["FunctionName"] for entry in item["Symbol"]}
		if address in samples:
			functions[frame["FunctionName"]] += samples[address]
			locations[(frame["FunctionName"], frame["FileName"], frame["Line"])] += samples[address]
	inclusive = collections.Counter()
	for key, stack in stacks.items():
		names = set().union(*(address_functions[address] for address in stack))
		for name in names:
			inclusive[name] += weights[key]
	delays.sort()
	output = {
		"pid": args.pid,
		"scheduled_cpu_seconds": sum(scheduled.values()) / 1e6,
		"thread_cpu_seconds": {str(thread): value / 1e6 for thread, value in scheduled.most_common()},
		"context_switch_mismatches_tracewide": mismatches,
		"module_sample_counts": dict(modules.most_common()),
		"application_function_sample_counts": dict(functions.most_common()),
		"application_stack_inclusive_samples": dict(inclusive.most_common()),
		"samples_with_application_stack": sum(weights[key] for key in stacks),
		"application_top_locations": [
			{"function": function, "file": file, "line": line, "samples": count}
			for (function, file, line), count in locations.most_common(30)
		],
		"switch_out_counts": {state + "/" + reason: count for (state, reason), count in states.most_common()},
		"observed_ready_delay_us": {
			"count": len(delays), "total": sum(delays),
			"median": delays[len(delays) // 2] if delays else None,
			"p95": delays[min(len(delays) - 1, int(len(delays) * .95))] if delays else None,
			"maximum": max(delays, default=None),
		},
		"thread_ready_delay_us": {
			str(thread): {"count": len(values), "total": sum(values), "maximum": max(values)}
			for thread, values in thread_ready.items()
		},
		"limitations": [
			"Samples are counts, not exact CPU durations; inline leaf attribution is exclusive.",
			"Stack-inclusive counts overlap and must not be summed; only stacks keyed to samples are counted.",
			"Scheduled intervals include interrupt time; compare with Xperf cswitch total.",
			"Ready delays cover observed ready events/preemption only, not blocked waits.",
			"No automatic native QPC/ETW alignment or SDK private-symbol resolution.",
		],
	}
	args.output.write_text(json.dumps(output, indent=2) + "\n", encoding="utf-8")
	print(json.dumps({key: output[key] for key in (
		"scheduled_cpu_seconds", "application_function_sample_counts", "application_stack_inclusive_samples", "observed_ready_delay_us"
	)}, indent=2))


if __name__ == "__main__":
	main()