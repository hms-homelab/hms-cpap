// SDD-059: what the log page holds, as plain data, so it is tested without a
// browser (log-view.spec.ts, run with `npm test`).
//
// The page holds the log as chunks, each one answer of /api/logs with the byte
// offsets around it. A refresh appends the lines written since the last chunk's
// end; scrolling to the top prepends the page before the first chunk's start.
// Past `cap` lines whole chunks are dropped from the far end, so every kept
// chunk still knows exactly where it sits in the file and the dropped lines can
// be asked for again.

export interface LogChunk {
  lines: string[];
  start: number;   // byte offset of the first line
  end: number;     // byte offset after the last complete line
}

/** The fields of a /api/logs answer this needs. */
export interface LogAnswer {
  lines: string[];
  start?: number;
  end?: number;
  file_id?: string;
  rotated?: boolean;
  more?: boolean;
}

export const VIEW_CAP = 20000;

export class LogView {
  chunks: LogChunk[] = [];
  fileId = '';
  readonly cap: number;

  // No parameter property: the spec runs under Node's own TypeScript support,
  // which strips types and refuses syntax that needs compiling.
  constructor(cap: number = VIEW_CAP) { this.cap = cap; }

  get lineCount(): number {
    return this.chunks.reduce((n, c) => n + c.lines.length, 0);
  }

  get start(): number { return this.chunks.length ? this.chunks[0].start : 0; }
  get end(): number { return this.chunks.length ? this.chunks[this.chunks.length - 1].end : 0; }
  /** Nothing older is left in this file. */
  get atStart(): boolean { return this.chunks.length > 0 && this.start === 0; }
  get empty(): boolean { return this.chunks.length === 0; }

  get text(): string {
    return this.chunks.map(c => c.lines.join('\n')).filter(t => t.length).join('\n');
  }

  /** A fresh open, or a rotation: the answer is all there is. */
  reset(a: LogAnswer): void {
    this.fileId = a.file_id ?? '';
    this.chunks = [];
    this.push(a, 'end');
  }

  /** What a refresh brought. A rotation replaces everything. */
  append(a: LogAnswer): void {
    if (a.rotated) { this.reset(a); return; }
    if (a.file_id) this.fileId = a.file_id;
    this.push(a, 'end');
    // Over the cap: the oldest chunks go. They can be loaded again by
    // scrolling up, from the start the next chunk keeps.
    while (this.chunks.length > 1 && this.lineCount > this.cap) this.chunks.shift();
  }

  /** The page before the first chunk. */
  prepend(a: LogAnswer): void {
    this.push(a, 'start');
    // Over the cap: the newest chunks go, and the next refresh asks from the
    // end the kept ones say, so they come back as they are needed.
    while (this.chunks.length > 1 && this.lineCount > this.cap) this.chunks.pop();
  }

  private push(a: LogAnswer, where: 'start' | 'end'): void {
    const c: LogChunk = {
      lines: a.lines ?? [],
      start: a.start ?? 0,
      end: a.end ?? a.start ?? 0,
    };
    if (where === 'end') {
      if (c.lines.length || this.chunks.length === 0) this.chunks.push(c);
      else this.chunks[this.chunks.length - 1].end = Math.max(this.end, c.end);
    } else if (c.lines.length) {
      this.chunks.unshift(c);
    }
  }
}
