'use strict';

function resourceTimers(state, run, failed) {
  const validate = (handler) => {
    if (typeof handler !== 'function') throw new TypeError('Timer handler must be a function');
    if (!state.active) throw new Error('Resource is stopped');
  };
  const invoke = (handler, args) => {
    if (!state.active) return;
    try {
      const result = run(() => handler(...args));
      if (result?.then)
        result.catch((error) => {
          if (state.active) failed(error);
        });
    } catch (error) {
      if (state.active) failed(error);
    }
  };
  const create = (repeat, handler, delay, args) => {
    validate(handler);
    let timer;
    const callback = () => {
      if (!repeat) state.timers.delete(timer);
      invoke(handler, args);
    };
    timer = repeat ? setInterval(callback, delay) : setTimeout(callback, delay);
    // A native Node Timeout can be revived with refresh after its first firing.
    // Keep that revival owned, and never revive a stopped resource's timeout.
    const refresh = timer.refresh.bind(timer);
    timer.refresh = () => {
      if (!state.active) return timer;
      state.timers.add(timer);
      return refresh();
    };
    state.timers.add(timer);
    return timer;
  };
  return {
    setTimeout: (handler, delay, ...args) => create(false, handler, delay, args),
    setInterval: (handler, delay, ...args) => create(true, handler, delay, args),
    clearTimeout: (timer) => {
      clearTimeout(timer);
      state.timers.delete(timer);
    },
    clearInterval: (timer) => {
      clearInterval(timer);
      state.timers.delete(timer);
    },
    queueMicrotask: (handler) => {
      validate(handler);
      queueMicrotask(() => invoke(handler, []));
    },
    // Node's setImmediate, owned like the other timers (bundled npm packages use it).
    setImmediate: (handler, ...args) => {
      validate(handler);
      const immediate = setImmediate(() => {
        state.timers.delete(immediate);
        invoke(handler, args);
      });
      state.timers.add(immediate);
      return immediate;
    },
    clearImmediate: (immediate) => {
      clearImmediate(immediate);
      state.timers.delete(immediate);
    },
  };
}

module.exports = { resourceTimers };
