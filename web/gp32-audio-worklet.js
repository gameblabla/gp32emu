class GP32AudioProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.queue = [];
    this.queued = 0;
    this.started = false;
    this.preroll = Math.floor(sampleRate * 0.12);
    this.lowWater = Math.floor(sampleRate * 0.05);
    this.maxQueue = Math.floor(sampleRate * 0.45);
    this.lastL = 0;
    this.lastR = 0;
    this.fade = 0;
    this.consumedSincePost = 0;
    this.underruns = 0;
    this.port.onmessage = (ev) => {
      const m = ev.data || {};
      if (m.type === 'reset') {
        this.queue.length = 0;
        this.queued = 0;
        this.started = false;
        this.lastL = this.lastR = 0;
        this.fade = 0;
        this.consumedSincePost = 0;
        this.underruns = 0;
      } else if (m.type === 'config') {
        if (m.preroll) this.preroll = m.preroll | 0;
        if (m.lowWater) this.lowWater = m.lowWater | 0;
        if (m.maxQueue) this.maxQueue = m.maxQueue | 0;
      } else if (m.type === 'audio' && m.data) {
        const data = m.data instanceof Int16Array ? m.data : new Int16Array(m.data);
        const frames = (m.frames | 0) || (data.length >> 1);
        if (frames > 0) {
          this.queue.push({ data, frames, pos: 0 });
          this.queued += frames;
          while (this.queued > this.maxQueue && this.queue.length > 2) {
            const h = this.queue.shift();
            this.queued -= h.frames - h.pos;
          }
        }
      }
    };
  }

  _popSample() {
    while (this.queue.length) {
      const h = this.queue[0];
      if (h.pos < h.frames) {
        const i = h.pos++ << 1;
        this.queued--;
        this.consumedSincePost++;
        if (h.pos >= h.frames) this.queue.shift();
        this.lastL = h.data[i] / 32768.0;
        this.lastR = h.data[i + 1] / 32768.0;
        return 1;
      }
      this.queue.shift();
    }
    return 0;
  }

  process(inputs, outputs) {
    const out = outputs[0];
    const l = out[0];
    const r = out[1] || out[0];
    if (!this.started) {
      if (this.queued >= this.preroll) this.started = true;
      else {
        l.fill(0); if (r !== l) r.fill(0);
        if (this.consumedSincePost || this.underruns) {
          this.port.postMessage({ type: 'stat', queued: this.queued, consumed: this.consumedSincePost, underruns: this.underruns, started: this.started });
          this.consumedSincePost = 0;
          this.underruns = 0;
        }
        return true;
      }
    }

    for (let i = 0; i < l.length; ++i) {
      if (this._popSample()) {
        l[i] = this.lastL;
        r[i] = this.lastR;
        this.fade = 0;
      } else {
        if (this.started) this.underruns++;
        /* Do not hard-cut to zero.  A short tail is less audible and avoids
           the click caused by a single late main-thread/WASM batch. */
        const gain = this.fade < 96 ? (1.0 - this.fade / 96.0) : 0.0;
        l[i] = this.lastL * gain;
        r[i] = this.lastR * gain;
        this.fade++;
        if (this.fade > 1024 && this.queued < this.lowWater) this.started = false;
      }
    }

    if (this.consumedSincePost >= 1024 || this.underruns) {
      this.port.postMessage({ type: 'stat', queued: this.queued, consumed: this.consumedSincePost, underruns: this.underruns, started: this.started });
      this.consumedSincePost = 0;
      this.underruns = 0;
    }
    return true;
  }
}
registerProcessor('gp32-audio-processor', GP32AudioProcessor);
