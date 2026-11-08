'use strict';

// A server's own loading page: LoadingScreen.show(url) puts a full-screen web view of
// a client resource over the game's load, from when the resources have started until the player's
// world is ready; package.json's kcds.loadingScreen shows it earlier, as soon as its own files have
// arrived, while the rest downloads. The game's own screen stays under it, and is what a player sees
// on a server that shows nothing.
//
// The page hears the load as a window event 'kcdsLoading', detail { stage, percent }: stage
// 'download' (the resources' bytes, with resourcesDone, resourcesTotal, bytesDone and bytesTotal),
// 'resources' (the resources have started, percent 0), 'level' (the game's own percentage) and
// 'ready' (100). Scripts, which run from 'resources' on, hear the same as the event
// loadingProgress(stage, percent). The page goes
// away at 'ready', or when LoadingScreen.release() is called if a script held it with
// LoadingScreen.hold(); a hold ends by itself after HOLD_LIMIT_MS, so a broken script cannot keep a
// player on it. While the server holds the player out of the world (Player.setLobby) the page
// stays, with no limit: the server lets the player in, and then the page goes as a release would.

const HOLD_LIMIT_MS = 30000;
// How long a download keeps the page up over a world that is already ready: one that never ends
// must not hide the game for good.
const DOWNLOAD_LIMIT_MS = 10 * 60 * 1000;
const EVENT = 'kcdsLoading';

class LoadingScreen {
  // web: the resource runtime's ResourceWebViews; emit(stage, percent) raises loadingProgress;
  // send reaches the game, which takes the server browser's window off the page.
  constructor({ web, emit, send = () => {} }) {
    this.web = web;
    this.emit = emit;
    this.send = send;
    this.stage = 'resources';
    this.percent = 0;
    this.shown = null; // { owner, viewId }
    this.held = false;
    this.holdTimer = null;
    // Whether the server holds the player out of the world (Player.setLobby).
    this.lobby = false;
    // The level loads beside the download: what it reached waits until the download ends.
    this.downloading = false;
    this.worldReady = false;
    this.levelPercent = 0;
    this.downloadTimer = null;
  }

  tellPage(extra = {}) {
    if (this.shown) {
      this.web
        .facade(this.shown.owner)
        .emit(this.shown.viewId, EVENT, { stage: this.stage, percent: this.percent, ...extra });
    }
  }

  tell() {
    this.tellPage();
    this.emit(this.stage, this.percent);
  }

  open(owner, url) {
    this.close();
    const viewId = this.web.facade(owner).open(url, { visible: true, focus: false });
    this.shown = { owner, viewId };
    this.send({ op: 'loadingPageShown' });
  }

  // A resource's kcds.loadingScreen: its own files have arrived.
  showManifestPage(owner, page) {
    this.open(owner, `kcds://resources/${owner}/${page}`);
    this.tellPage();
  }

  // The resources' download, for the page: { resourcesDone, resourcesTotal, bytesDone, bytesTotal }.
  download(progress) {
    if (this.stage === 'ready') {
      return;
    }
    this.downloading = true;
    this.stage = 'download';
    const { bytesDone, bytesTotal } = progress;
    this.percent = bytesTotal > 0 ? Math.floor((bytesDone / bytesTotal) * 100) : 100;
    this.tellPage(progress);
  }

  // The download is over and the resources start.
  resourcesStarting() {
    this.downloading = false;
    clearTimeout(this.downloadTimer);
    this.downloadTimer = null;
    if (this.stage === 'ready') {
      return;
    }
    this.stage = 'resources';
    this.percent = 0;
    this.tellPage();
    if (this.worldReady) {
      this.ready();
    } else if (this.levelPercent > 0) {
      this.progress(this.levelPercent);
    }
  }

  // The player left: the next connection starts over.
  reset() {
    this.close();
    clearTimeout(this.downloadTimer);
    this.downloadTimer = null;
    this.stage = 'resources';
    this.percent = 0;
    this.downloading = false;
    this.worldReady = false;
    this.levelPercent = 0;
    this.lobby = false;
  }

  // The game's own percentage of the level's load.
  progress(percent) {
    if (this.stage === 'ready' || !Number.isFinite(percent)) {
      return;
    }
    if (this.downloading) {
      this.levelPercent = Math.max(this.levelPercent, percent);
      return;
    }
    const whole = Math.max(this.percent, Math.min(100, Math.max(0, Math.round(percent))));
    if (this.stage === 'level' && whole === this.percent) {
      return;
    }
    this.stage = 'level';
    this.percent = whole;
    this.tell();
  }

  // The world is ready and the game's own screen went away.
  ready() {
    if (this.downloading) {
      this.worldReady = true;
      this.downloadTimer ??= setTimeout(() => {
        this.downloading = false;
        this.ready();
      }, DOWNLOAD_LIMIT_MS);
      return;
    }
    this.worldReady = false;
    this.stage = 'ready';
    this.percent = 100;
    this.tell();
    if (!this.held && !this.lobby) {
      this.close();
    }
  }

  // The server holds the player out of the world (Player.setLobby) or lets them in. Let in, the
  // page goes once the world is ready and no script holds it.
  setLobby(held) {
    this.lobby = held;
    if (!held && this.stage === 'ready' && !this.held) {
      this.close();
    }
  }

  close() {
    clearTimeout(this.holdTimer);
    this.holdTimer = null;
    this.held = false;
    const shown = this.shown;
    this.shown = null;
    if (shown) {
      this.web.facade(shown.owner).close(shown.viewId);
      // The game client may show its own windows again.
      this.send({ op: 'loadingPageClosed' });
    }
  }

  facade(owner) {
    const self = this;
    return Object.freeze({
      // Shows the page; a page shown before it goes. False once the world is ready, unless the
      // server holds the player out of the world.
      show(url) {
        if (self.stage === 'ready' && !self.lobby) {
          return false;
        }
        self.open(owner, url);
        return true;
      },
      // Keeps the page up after the world is ready, until release() or HOLD_LIMIT_MS.
      hold() {
        if (!self.shown || self.held) {
          return false;
        }
        self.held = true;
        self.holdTimer = setTimeout(() => {
          self.held = false;
          self.holdTimer = null;
          // A player the server holds out of the world keeps the page.
          if (!self.lobby) {
            self.close();
          }
        }, HOLD_LIMIT_MS);
        return true;
      },
      // Ends a hold: at 'ready' the page goes now.
      release() {
        if (!self.held) {
          return false;
        }
        clearTimeout(self.holdTimer);
        self.holdTimer = null;
        self.held = false;
        if (self.stage === 'ready' && !self.lobby) {
          self.close();
        }
        return true;
      },
      get stage() {
        return self.stage;
      },
      get percent() {
        return self.percent;
      },
    });
  }
}

module.exports = { LoadingScreen, HOLD_LIMIT_MS, DOWNLOAD_LIMIT_MS };
