# The launcher's update notice (2026-11-13)

- **What:** when a release newer than the launcher's own version is out on GitHub, pre-releases too, the launcher says
  so: a notice over the Play tab's header (`NEW PRE-RELEASE`, `0.1.0-alpha.8 is out`, Download, a cross), and the
  number in gold under its own in the sidebar (`0.1.0-alpha.8 available ›`, in place of the day it was built). Download
  and the sidebar's line open the release's page in the browser: the Kit and its notes are there. The cross hides the
  notice for that release; the sidebar's line stays until the update.
- **Asking:** `GET https://api.github.com/repos/rigaz29/Ascension-R/releases?per_page=30` with WinHTTP, as the RPCS3
  download (`port/game_prepare.cpp`): Windows' proxy settings, 5 s for each step, on a thread of its own
  (`port/update_check.cpp`). Not `/releases/latest`: that one leaves pre-releases out, and every release so far is one,
  so it answers 404. GitHub wants a User-Agent: `Ascension-R/<version>`, and nothing else is sent. A client with no
  account may ask 60 times an hour; this asks at most once every six hours. No answer (no network, a proxy, the limit):
  nothing is said, and the next start asks again.
- **Reading the answer** (`port/update_feed.cpp`, no Windows in it): the list is an array of releases, each an object;
  only a release's own keys count (`tag_name`, `html_url`, `draft`, `prerelease`), as `html_url` comes again in its
  author and every asset. Strings are read through their escapes, so braces or quotes in the notes do not count.
  Drafts are left out, and so is a release whose page is not under `https://github.com/rigaz29/Ascension-R/releases/`
  (only such a page is ever opened). Versions: `MAJOR.MINOR.PATCH[-label.N]`, a `v` before it or not; a pre-release
  comes before its release, labels in their order as text (alpha, beta, rc), then the number. A tag that is not a
  version (`nightly`, `0.1.0-alpha.8-dev`) is left out.
- **Kept:** `update_check.txt` beside the settings file: when it asked, the newer release it found and the one whose
  notice was closed. The notice shows from it at once at the next start, and the settings file is never written behind
  the player's back.
- **Setting:** Check for updates, in the System tab (`launcher.update_check`, on by default). Off: no question, no notice.
  A game started with `--no-launcher` (scripts, benches) never asks.
- **Trying it:** `GOWA_UPDATE_FEED=<file>` reads the list from a file, every start; `GOWA_LAUNCHER_SHOT` draws the Play
  tab with the next pre-release known as its last picture. `tests/test_update_check.py` (CI): versions read and put in
  order, the newest past this one in GitHub-shaped lists, drafts, pages elsewhere, tags that are not versions, notes
  with braces and quotes, a file with a UTF-8 mark, an error object, a list cut short: 11 cases.
- **Tried here**, the build's own window driven by posted messages, once:

  | Step | `update_check.txt` after | The window |
  |---|---|---|
  | GitHub itself (its newest is 0.1.0-alpha.7, this one) | `checked <time>` | nothing |
  | A list with 0.1.0-alpha.8 (`GOWA_UPDATE_FEED`) | the same, and `latest v0.1.0-alpha.8 <page>` | the notice and the sidebar's line |
  | The cross | the same, and `dismissed v0.1.0-alpha.8` | the sidebar's line |
  | Opened again on the list | as it was | the sidebar's line |

  The settings file's hash was the same before and after.
- **Limits:** it only says and links: nothing is downloaded or replaced (the Kit builds on the player's PC, and files in
  use cannot be swapped under the launcher). It starts with the version that carries it: players of 0.1.0-alpha.7 and
  before are not told. A build from source says its tree's version, so it is told of a release until that version is
  raised.
