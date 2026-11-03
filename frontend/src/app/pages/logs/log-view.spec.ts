// SDD-059: the log page's view, without a browser. `npm test` runs it with
// Node's own test runner and TypeScript support; nothing is installed for it.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { LogView } from './log-view.ts';

const lines = (from: number, to: number) =>
  Array.from({ length: to - from }, (_, i) => `line ${from + i}`);

test('a fresh open holds the answer and its offsets', () => {
  const v = new LogView();
  v.reset({ lines: lines(0, 3), start: 100, end: 160, file_id: 'f1' });
  assert.equal(v.lineCount, 3);
  assert.equal(v.start, 100);
  assert.equal(v.end, 160);
  assert.equal(v.fileId, 'f1');
  assert.equal(v.atStart, false);
  assert.equal(v.text, 'line 0\nline 1\nline 2');
});

test('a refresh appends what is new, and an empty one only moves nothing', () => {
  const v = new LogView();
  v.reset({ lines: lines(0, 2), start: 0, end: 20, file_id: 'f1' });
  v.append({ lines: [], start: 20, end: 20, file_id: 'f1' });
  assert.equal(v.chunks.length, 1);
  v.append({ lines: lines(2, 4), start: 20, end: 40, file_id: 'f1' });
  assert.equal(v.lineCount, 4);
  assert.equal(v.end, 40);
  assert.equal(v.text.split('\n').at(-1), 'line 3');
});

test('a rotation replaces everything', () => {
  const v = new LogView();
  v.reset({ lines: lines(0, 5), start: 0, end: 50, file_id: 'f1' });
  v.append({ lines: lines(100, 102), start: 0, end: 22, file_id: 'f2', rotated: true });
  assert.equal(v.lineCount, 2);
  assert.equal(v.fileId, 'f2');
  assert.equal(v.text, 'line 100\nline 101');
});

test('scrolling up prepends the page before and knows when the file starts', () => {
  const v = new LogView();
  v.reset({ lines: lines(10, 20), start: 100, end: 200, file_id: 'f1' });
  v.prepend({ lines: lines(0, 10), start: 0, end: 100 });
  assert.equal(v.lineCount, 20);
  assert.equal(v.start, 0);
  assert.equal(v.atStart, true);
  assert.equal(v.text.split('\n')[0], 'line 0');
  v.prepend({ lines: [], start: 0, end: 0 });   // nothing older: nothing changes
  assert.equal(v.chunks.length, 2);
});

test('past the cap the oldest chunks go on append, keeping exact offsets', () => {
  const v = new LogView(10);
  v.reset({ lines: lines(0, 6), start: 0, end: 60, file_id: 'f1' });
  v.append({ lines: lines(6, 10), start: 60, end: 100, file_id: 'f1' });
  v.append({ lines: lines(10, 13), start: 100, end: 130, file_id: 'f1' });
  assert.equal(v.lineCount, 7);           // the first chunk of 6 went
  assert.equal(v.start, 60);              // and the view starts where the next one did
  assert.equal(v.end, 130);
  assert.equal(v.atStart, false);         // so scrolling up can ask for it again
});

test('past the cap the newest chunks go on prepend, and the end says so', () => {
  const v = new LogView(15);
  v.reset({ lines: lines(10, 16), start: 100, end: 160, file_id: 'f1' });
  v.append({ lines: lines(16, 18), start: 160, end: 180, file_id: 'f1' });
  v.prepend({ lines: lines(2, 10), start: 20, end: 100 });   // 16 lines: one over
  assert.equal(v.lineCount, 14);          // the newest chunk of 2 went
  assert.equal(v.start, 20);
  assert.equal(v.end, 160);               // the next refresh asks from here
});

test('a single chunk larger than the cap is kept whole', () => {
  const v = new LogView(5);
  v.reset({ lines: lines(0, 8), start: 0, end: 80, file_id: 'f1' });
  assert.equal(v.lineCount, 8);
});
