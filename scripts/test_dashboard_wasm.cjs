const assert = require("node:assert/strict");
const createModule = require("../docs/kernel.js");

(async () => {
  const m = await createModule();
  assert.equal(m._MK_Init(), 0, "kernel initialization");
  for (let i = 0; i < 12; i++) m._MK_Step();
  assert.equal(m._MK_GetTaskCount(), 5, "five task control blocks");
  assert.ok(m._MK_GetTick() >= 12, "scheduler ticks advance");
  assert.ok(m._MK_GetContextSwitches() > 0, "scheduler switches tasks");
  assert.equal(m._MK_MutexScenario(), 0, "mutex contention and ownership handoff");
  assert.equal(m._MK_IPCScenario(), 0, "IPC send, wake and receive");
  assert.equal(m._MK_InjectWatchdog(), 0, "watchdog detection and verified recovery");
  assert.equal(m._MK_GetTaskRecoveries(3), 1, "recovery count recorded");
  assert.equal(m._MK_InjectIsolation(), 0, "recovery limit isolates task");
  assert.equal(m._MK_GetTaskIsolated(4), 1, "isolated state recorded");
  const allocated = [];
  for (let i = 0; i < 8; i++) allocated.push(m._MK_AllocateBlock());
  assert.ok(allocated.every(i => i >= 0), "all memory blocks allocate");
  assert.equal(m._MK_AllocateBlock(), -1, "pool exhaustion is reported");
  assert.equal(m._MK_FreeBlock(allocated[0]), 0, "allocated block can be freed");
  assert.ok(m._MK_GetEventCount() > 0, "kernel trace events are retained");
  console.log("PASS: WebAssembly kernel initialization, scheduler, mutex, IPC, watchdog recovery, isolation, memory pool and trace.");
})().catch(error => { console.error(error); process.exitCode = 1; });
