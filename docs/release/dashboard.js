/* The Release Command Center's script (#4524). See index.html.
 *
 * Reads index.json and <milestone>.json (tools/release-dashboard.py writes them) and draws the page.
 * Every string from the data is someone else's text (issue and pull-request titles, comments), so nodes
 * are built with textContent and links are built here from numbers, never taken from the data as HTML. */
(function () {
  'use strict';

  const SVG_NS = 'http://www.w3.org/2000/svg';
  const GITHUB = 'https://github.com/';
  const TITLE = /^v\d+\.\d+\.\d+$/;
  const REPO = /^[\w.-]+\/[\w.-]+$/;
  const REFRESH_MS = 5 * 60 * 1000;
  const STALE_MINUTES = 45; // the schedule is every 10 minutes; GitHub can start a run a quarter of an hour late
  const HOUR = 3600;
  const DAY = 86400;

  const STATE_WORDS = { done: 'Done', active: 'In progress', todo: 'Not started', blocked: 'Blocked' };
  const PHASE_WORDS = { todo: 'Not started', progress: 'In progress', review: 'In review', done: 'Done' };
  const PHASE_ICONS = { todo: 'todo', progress: 'active', review: 'active', done: 'done' };
  // docs/dev/labels.md's queue order.
  const CATEGORIES = ['CI', 'Tooling', 'Security', 'Bug', 'Perf', 'Testing', 'Docs', 'Refactor', 'Parity', 'Feature', 'Other'];
  const LANES = ['Windows', 'WSL', 'Shared', 'Dash', 'None'];
  // Maps, not objects: a lane's name comes from a label, and "constructor" must not find Object's.
  const LANE_WORDS = new Map([['Shared', 'Both lanes'], ['None', 'No lane']]);
  // The GitHub search that lists a lane's issues (#4585), by the generator's own rule (lane() in
  // tools/release-dashboard.py): one agent:* label is that lane, two or more are shared. So a lane's search
  // leaves the other lanes' labels out, and the shared one asks for any two. "No lane" has none: its search
  // would list the tracker too. A new agent:* label needs a line here.
  const LANE_SEARCH = new Map([
    ['Windows', 'label:agent:Windows -label:agent:WSL -label:agent:Dash'],
    ['WSL', 'label:agent:WSL -label:agent:Windows -label:agent:Dash'],
    ['Shared', '((label:agent:Windows AND label:agent:WSL) OR (label:agent:Windows AND label:agent:Dash) OR (label:agent:WSL AND label:agent:Dash))'],
    ['Dash', 'label:agent:Dash -label:agent:Windows -label:agent:WSL'],
  ]);
  const LEVELS = new Map([['high', 0], ['medium', 1], ['low', 2]]);
  const LIST_BY = [['kind', 'Kind'], ['lane', 'Lane']];
  const LIST_BY_KEY = 'release-dashboard-list-by';
  const STAGES = [
    ['building', 'Building', 'Foundation, fixes, features'],
    ['freeze', 'Feature freeze', 'Review, hardening, docs'],
    ['candidate', 'Candidate', 'Approved, final checks'],
    ['released', 'Released', ''],
  ];

  /** What this browser kept under @p key, or null: storage can be blocked, or refuse in a private window. */
  function remembered(key) {
    try {
      return localStorage.getItem(key);
    } catch (error) {
      return null;
    }
  }

  function remember(key, value) {
    try {
      localStorage.setItem(key, value);
    } catch (error) {
      // The choice then lasts for this visit only.
    }
  }

  const app = document.getElementById('app');
  const base = (document.querySelector('meta[name="release-data"]') || {}).content || '';
  const view = { index: null, release: null, charts: [], width: 0, listBy: remembered(LIST_BY_KEY) === 'lane' ? 'lane' : 'kind' };

  // ---- small helpers -----------------------------------------------------------------------------

  function fill(node, props, children, set) {
    for (const [key, value] of Object.entries(props || {})) {
      if (value == null || value === false) continue;
      if (key === 'text') node.textContent = String(value);
      else if (key.startsWith('on')) node.addEventListener(key.slice(2), value);
      else set(key, value === true ? '' : String(value));
    }
    for (const child of children.flat(3)) {
      if (child == null || child === false) continue;
      node.append(child.nodeType ? child : document.createTextNode(String(child)));
    }
    return node;
  }

  function el(tag, props, ...children) {
    const node = document.createElement(tag);
    return fill(node, props, children, (key, value) => node.setAttribute(key, value));
  }

  function svg(tag, props, ...children) {
    const node = document.createElementNS(SVG_NS, tag);
    return fill(node, props, children, (key, value) => node.setAttribute(key, value));
  }

  const seconds = (iso) => (iso ? Date.parse(iso) / 1000 : null);
  const dateFormat = new Intl.DateTimeFormat(undefined, { month: 'short', day: 'numeric' });
  const timeFormat = new Intl.DateTimeFormat(undefined, { month: 'short', day: 'numeric', hour: 'numeric', minute: '2-digit' });
  const number = new Intl.NumberFormat();
  const day = (s) => dateFormat.format(new Date(s * 1000));
  const moment = (s) => timeFormat.format(new Date(s * 1000));

  /** A length of time, from hours: 18 min, 5.2 h, 3.1 d. */
  function span(hours) {
    if (hours == null) return '–';
    if (hours < 1) return Math.max(1, Math.round(hours * 60)) + ' min';
    if (hours < 48) return (Math.round(hours * 10) / 10) + ' h';
    return (Math.round((hours / 24) * 10) / 10) + ' d';
  }

  function ago(s, now) {
    const hours = (now - s) / HOUR;
    return hours < 0.02 ? 'just now' : span(hours) + ' ago';
  }

  function days(value) {
    if (value == null) return '–';
    return value < 1 ? 'under a day' : value === 1 ? '1 day' : value + ' days';
  }

  const count = (n, one, many) => number.format(n) + ' ' + (n === 1 ? one : many || one + 's');
  const signed = (n) => (n > 0 ? '+' : n < 0 ? '−' : '') + Math.abs(n);

  function repoUrl() {
    const repo = view.index && view.index.repo;
    return REPO.test(repo || '') ? GITHUB + repo : null;
  }

  function link(text, path, props) {
    const root = repoUrl();
    return root ? el('a', { href: root + path, ...props }, text) : el('span', props, text);
  }

  /** A URL from the data (a comment, a run), used only when it's on this repository. */
  function safeUrl(url) {
    const root = repoUrl();
    return root && typeof url === 'string' && url.startsWith(root + '/') ? url : null;
  }

  const issueLink = (n) => link('#' + n, '/issues/' + Number(n), { class: 'item-number' });
  const laneName = (name) => LANE_WORDS.get(name) || name;

  /** The names in @p groups: the ones @p order knows in its order, then any others by name. */
  function ordered(order, groups) {
    return order.filter((name) => groups.has(name)).concat(Array.from(groups.keys()).filter((name) => !order.includes(name)).sort());
  }

  function icon(state) {
    const node = svg('svg', { class: 'icon icon-' + state, viewBox: '0 0 18 18', 'aria-hidden': 'true' });
    if (state === 'done') {
      node.append(svg('circle', { cx: 9, cy: 9, r: 8, fill: 'currentColor' }), svg('path', { class: 'cut', d: 'M5.2 9.3l2.6 2.6 5-5.4' }));
    } else if (state === 'blocked') {
      node.append(svg('circle', { cx: 9, cy: 9, r: 8, fill: 'currentColor' }), svg('path', { class: 'cut', d: 'M6 6l6 6M12 6l-6 6' }));
    } else if (state === 'active') {
      node.append(
        svg('circle', { cx: 9, cy: 9, r: 7, fill: 'none', stroke: 'currentColor', 'stroke-width': 2 }),
        svg('path', { d: 'M9 2a7 7 0 0 1 0 14z', fill: 'currentColor' })
      );
    } else {
      node.append(svg('circle', { cx: 9, cy: 9, r: 7, fill: 'none', stroke: 'currentColor', 'stroke-width': 2 }));
    }
    return node;
  }

  const chip = (text, strong) => el('span', { class: strong ? 'chip strong' : 'chip', text });

  function details(key, className, summary, body, open) {
    return el('details', { class: className, 'data-key': key, open: open }, summary, body);
  }

  // ---- loading -----------------------------------------------------------------------------------

  async function load(name) {
    // A test or a preview can hand the page its data instead of the network.
    if (window.__RELEASE_DATA__) {
      if (!window.__RELEASE_DATA__[name]) throw new Error(name + ' is missing');
      return window.__RELEASE_DATA__[name];
    }
    const response = await fetch(base + name, { cache: 'no-cache' });
    if (!response.ok) throw new Error(name + ': HTTP ' + response.status);
    return response.json();
  }

  function wanted(index) {
    let asked = null;
    try {
      asked = new URLSearchParams(location.search).get('m');
    } catch (error) {
      asked = null;
    }
    const titles = index.releases.map((release) => release.title);
    if (asked && TITLE.test(asked) && titles.includes(asked)) return asked;
    return titles.includes(index.current) ? index.current : titles[titles.length - 1];
  }

  async function show(title, quiet) {
    try {
      const index = await load('index.json');
      const pick = title || (view.release && view.release.milestone.title) || wanted(index);
      if (!TITLE.test(pick || '')) throw new Error('no release to show');
      if (quiet && view.release && view.index && view.index.generated_at === index.generated_at) return;
      const release = await load(pick + '.json');
      view.index = index;
      view.release = release;
      render();
    } catch (error) {
      if (view.release) {
        // Keep what's on screen (a failed switch puts the picker back on it); the next refresh tries again.
        if (!quiet) render();
        return;
      }
      app.replaceChildren(
        el('p', { class: 'notice' }, "The release data couldn't be loaded (" + String(error.message || error) + '). ',
          el('a', { href: GITHUB + 'mgradwohl/tasksmack/milestones' }, 'The milestones page'), ' has the same scope.')
      );
    }
  }

  // ---- the page ----------------------------------------------------------------------------------

  function render() {
    const release = view.release;
    const now = seconds(release.generated_at);
    const open = new Set(Array.from(app.querySelectorAll('details[open]'), (node) => node.dataset.key));
    const first = !app.querySelector('details');
    view.charts = [];

    // The two things that depend on the clock, not on the data: how long ago it was generated, and the
    // warning when that's too long. clock() keeps them right on a page left open.
    const stamp = el('span', { title: moment(now) });
    const age = el('span');
    const stale = el('p', { class: 'banner', hidden: true }, 'This data is ', age, ' old. It should refresh every 10 minutes: check the ',
      link('Release Dashboard workflow', '/actions/workflows/release-dashboard.yml'), '.');
    view.clock = () => {
      const minutes = (Date.now() / 1000 - now) / 60;
      stamp.textContent = 'Updated ' + ago(now, Date.now() / 1000);
      age.textContent = span(minutes / 60);
      stale.hidden = Boolean(window.__RELEASE_DATA__) || release.stage === 'released' || minutes <= STALE_MINUTES;
    };
    const parts = [masthead(release, stamp), stages(release), stale];
    if (release.stage !== 'planned') parts.push(summary(release));
    parts.push(shipList(release, now));
    if (release.stage !== 'planned') parts.push(movement(release, now));
    if (release.open_issues.length) parts.push(openWork(release, now));
    if (release.stage !== 'planned') parts.push(mainHealth(release, now));
    parts.push(compare(release), footer(release));
    app.replaceChildren(...parts);

    if (!first) {
      for (const node of app.querySelectorAll('details')) node.open = open.has(node.dataset.key);
    }
    view.clock();
    view.width = app.clientWidth;
    for (const draw of view.charts) draw();
  }

  function masthead(release, stamp) {
    const select = el('select', { id: 'release-select', onchange: (event) => choose(event.target.value) },
      view.index.releases.map((entry) => el('option', { value: entry.title, selected: entry.title === release.milestone.title },
        entry.title + (entry.stage === 'released' ? '' : entry.stage === 'planned' ? ' (planned)' : ' (in progress)'))));
    const links = [link('Milestone', '/milestone/' + Number(release.milestone.number))];
    if (release.tracker) links.push(link('Release tracker #' + release.tracker.number, '/issues/' + Number(release.tracker.number)));
    links.push(link('Release process', '/blob/main/docs/dev/release-process.md'));
    return el('header', { class: 'masthead' },
      el('div', null, el('p', { class: 'eyebrow', text: 'TaskSmack · Release Command Center' }), el('h1', { text: release.milestone.title }),
        el('p', { class: 'links' }, links)),
      el('div', { class: 'masthead-side' },
        el('label', { for: 'release-select' }, 'Release', select),
        stamp));
  }

  /** The release's goal: the milestone's due date. It's a calendar day, whatever time GitHub stores with it. */
  function goal(release) {
    const match = /^(\d{4})-(\d\d)-(\d\d)/.exec(release.milestone.due_on || '');
    if (!match) return null;
    const end = new Date(Number(match[1]), Number(match[2]) - 1, Number(match[3]), 23, 59, 59);
    return { end: end.getTime() / 1000, label: dateFormat.format(end) };
  }

  function choose(title) {
    if (!TITLE.test(title)) return;
    try {
      history.replaceState(null, '', '?m=' + encodeURIComponent(title));
    } catch (error) {
      // Some hosts don't allow it; the page still switches.
    }
    show(title);
  }

  function stages(release) {
    const at = {
      building: release.cycle.start,
      freeze: release.signals.freeze && release.signals.freeze.at,
      candidate: release.signals.approval && release.signals.approval.at,
      released: release.cycle.end,
    };
    const position = STAGES.findIndex((stage) => stage[0] === release.stage);
    const target = goal(release);
    return el('ol', { class: 'stages', 'aria-label': 'Release stage' }, STAGES.map(([key, name, note], index) => {
      const when = at[key] ? 'since ' + day(seconds(at[key])) : note;
      // Under "Released": the goal date beside the date it really shipped, or TBD until it does.
      const notes = key === 'released'
        ? [target ? el('small', { class: 'goal', text: 'Goal: ' + target.label }) : null,
          at.released || target ? el('small', { text: 'Actual: ' + (at.released ? day(seconds(at.released)) : 'TBD') }) : null]
        : [el('small', { text: release.stage === 'planned' && index === 0 ? 'Not started' : when })];
      return el('li', { class: index < position ? 'past' : index === position ? 'now' : null, 'aria-current': index === position ? 'step' : null },
        name, notes);
    }));
  }

  function summary(release) {
    const total = release.counts.issues;
    const open = release.counts.open;
    const closed = total - open;
    const percent = total ? Math.round((100 * closed) / total) : 0;
    const bar = el('span');
    bar.style.width = percent + '%';
    // Two rows (#4548): the large figure and its split by lane, then the sentences beside the pace table, so the
    // table's caption is level with the line under the figure whatever the figure's size.
    const top = el('div', { class: 'hero-top' }, el('p', { class: 'hero-figure', text: number.format(open) }), open ? laneFigures(release) : null);
    const hero = el('div', { class: 'hero' },
      el('p', { class: 'hero-label' }, open === 1 ? 'issue open of ' : 'issues open of ', number.format(total), ' on the milestone · ', percent + '% closed'),
      el('div', { class: 'meter', role: 'img', 'aria-label': percent + '% of the milestone issues are closed' }, bar));
    // GitHub's milestone page counts more than this page does: say what, so the two figures can be reconciled.
    const uncounted = el('p', { class: 'footnote' }, release.tracker
      ? ['The release tracker, ', link('#' + release.tracker.number, '/issues/' + Number(release.tracker.number)), ', and pull requests aren’t counted here. GitHub’s milestone page counts both, so its figures are higher.']
      : 'Pull requests aren’t counted here. GitHub’s milestone page counts them, so its figures can be higher.');
    if (!release.pace) {
      hero.append(uncounted);
      return el('section', { class: 'summary' }, top, hero);
    }

    const three = release.pace.find((rate) => rate.days === 3) || release.pace[0];
    const one = release.pace.find((rate) => rate.days === 1);
    let sentence;
    if (!open) sentence = 'Every milestone issue is closed.';
    else if (three.days_left != null) sentence = "At the last 3 days' pace, the open issues would take about " + days(three.days_left) + '.';
    else if (three.net_per_day === 0) sentence = "Over the last 3 days the open count didn't move: " + three.added_per_day + ' added and ' + three.closed_per_day + ' closed a day.';
    else sentence = 'Over the last 3 days, more issues were added than closed: ' + three.added_per_day + ' added and ' + three.closed_per_day + ' closed a day.';
    if (open && three.days_left == null && one && one.days_left != null) sentence += ' The last 24 hours did better: about ' + days(one.days_left) + ' at that rate.';
    hero.append(el('p', { class: 'pace-line', text: sentence }));
    const target = goal(release);
    if (target && open) {
      const left = (target.end - seconds(release.generated_at)) / DAY;
      const week = release.pace.find((rate) => rate.days === 7) || three;
      const away = left < 1 ? 'today' : left < 2 ? 'tomorrow' : Math.floor(left) + ' days away';
      hero.append(el('p', { class: 'pace-line', text: left > 0
        ? 'The goal is ' + target.label + ', ' + away + '. Closing what is open by then takes a net ' + (Math.round((10 * open) / Math.max(left, 1)) / 10) + ' a day; the last 7 days ran at ' + signed(week.net_per_day) + '.'
        : 'The goal was ' + target.label + ', ' + days(Math.round(-left)) + ' ago.' }));
    }
    hero.append(uncounted);

    const table = el('table', null,
      el('caption', { text: 'Pace, in issues a day' }),
      el('thead', null, el('tr', null, ['Over the last', 'Closed', 'Added', 'Net', 'Time left'].map((text) => el('th', { scope: 'col', text })))),
      el('tbody', null, release.pace.map((rate) => el('tr', null,
        el('th', { scope: 'row', text: rate.days === 1 ? '24 hours' : rate.days + ' days' }),
        el('td', { text: rate.closed_per_day }), el('td', { text: rate.added_per_day }),
        el('td', { text: signed(rate.net_per_day) }), el('td', { text: rate.days_left == null ? 'not converging' : days(rate.days_left) })))));
    return el('section', { class: 'summary' }, top, hero,
      el('div', { class: 'block pace' }, el('div', { class: 'scroll' }, table),
        el('p', { class: 'footnote', text: 'Arithmetic on recent days, not a forecast. A review sweep that files many findings at once turns the net negative for a while.' })));
  }

  /**
   * The open count split by lane (#4585): Windows and WSL always, any other lane only while it has open issues,
   * so the figures add up to the large one beside them. Each links to the same list on GitHub.
   */
  function laneFigures(release) {
    if (!release.by_lane) return null;
    // The data leaves out a lane with nothing open and nothing moving in a week: the two lanes still get a 0.
    const lanes = new Map(['Windows', 'WSL'].map((name) => [name, { name, open: 0 }]));
    for (const entry of release.by_lane) lanes.set(entry.name, entry);
    const title = release.milestone.title;
    const names = ordered(LANES, lanes).filter((name) => name === 'Windows' || name === 'WSL' || lanes.get(name).open);
    return el('ul', { class: 'lanes', 'aria-label': 'Open issues by lane' }, names.map((name) => {
      const entry = lanes.get(name);
      const search = TITLE.test(title) && LANE_SEARCH.get(name);
      // The space is for a screen reader ("9 Windows"): the two are stacked, so it isn't drawn.
      const parts = [el('span', { class: 'lane-figure', text: number.format(entry.open) }), ' ', el('span', { class: 'lane-name', text: laneName(name) })];
      return el('li', { title: entry.open ? entry.todo + ' not started, ' + entry.progress + ' in progress, ' + entry.review + ' in review' : null },
        search ? link(parts, '/issues?q=' + encodeURIComponent('is:issue is:open milestone:' + title + ' ' + search)) : parts);
    }));
  }

  // ---- the ship list -----------------------------------------------------------------------------

  function shipList(release, now) {
    const byNumber = new Map(release.open_issues.map((issue) => [issue.n, issue]));
    const rows = release.ship_list.map((row) => {
      let body = null;
      if (row.issues && row.issues.length) {
        body = issueList(row.issues.map((n) => byNumber.get(n)).filter(Boolean), now);
      } else if (row.id === 'checklist') {
        body = checklist(release);
      } else if (row.id === 'pull_requests' && release.live && release.live.pull_requests.length) {
        body = pullRequests(release, now);
      } else if (row.id === 'main' && release.live) {
        body = workflows(release, now);
      }
      const meta = el('div', { class: 'row-meta' }, el('span', { class: 'state-word', text: STATE_WORDS[row.state] || row.state }));
      if (row.moved) {
        meta.append(el('span', { text: 'Last 24 h: ' + row.moved.closed_24h + ' closed, ' + row.moved.added_24h + ' added' }),
          el('span', { text: '7 days: ' + row.moved.closed_7d + ' closed, ' + row.moved.added_7d + ' added' }));
      }
      if (row.note) meta.append(el('span', { text: row.note }));
      const url = safeUrl(row.link);
      if (url && !body) meta.append(el('a', { href: url, text: 'Open on GitHub' }));
      const head = [icon(row.state), el('span', { class: 'row-title' }, row.title, body ? el('span', { class: 'chevron', 'aria-hidden': 'true' }) : null),
        el('span', { class: 'row-summary', text: tidy(row.summary) }), meta];
      if (!body) return el('div', { class: 'row-static' }, head);
      const inner = el('div', { class: 'row-body' }, body, url ? el('a', { href: url, text: 'Open on GitHub' }) : null);
      return details('ship-' + row.id, 'row', el('summary', null, head), inner, false);
    });
    const blocked = release.ship_list.filter((row) => row.state === 'blocked');
    return el('section', null,
      el('div', { class: 'section-head' }, el('h2', { text: release.stage === 'released' ? 'What shipping took' : 'Before it ships' }),
        el('p', { class: 'sub', text: "In the order the release process reaches them. Each line's state comes from the milestone, the tracker and CI." })),
      blocked.length ? el('p', { class: 'banner attention' }, el('strong', { text: 'Blocked now: ' }),
        blocked.map((row, index) => [index ? '; ' : '', row.title + ' (' + tidy(row.summary) + ')']), '.') : null,
      el('div', { class: 'ship' }, rows));
  }

  /** Summaries carry ISO times ("declared 2026-11-01T14:34:55Z"); show them in the reader's time. */
  function tidy(text) {
    return String(text).replace(/\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ/g, (iso) => moment(seconds(iso)));
  }

  const level = (value) => LEVELS.get(value) ?? 3;

  /** Most severe first, then the highest priority, then the oldest. */
  function byUrgency(a, b) {
    return level(a.sev) - level(b.sev) || level(a.pri) - level(b.pri) || a.n - b.n;
  }

  /**
   * The order a lane takes its work in (docs/dev/labels.md, "Queue order"): the steps in turn, with security
   * and bugs as one step; within a step the highest priority first, then the most severe, then the oldest.
   * The data's kind already puts a sev:high or sev:medium item with the bugs.
   */
  function byQueue(a, b) {
    const step = (issue) => (CATEGORIES.indexOf(issue.category === 'Security' ? 'Bug' : issue.category) + 1) || CATEGORIES.length + 1;
    return step(a) - step(b) || level(a.pri) - level(b.pri) || level(a.sev) - level(b.sev) || a.n - b.n;
  }

  /**
   * The open issues that wait on the maintainer, as the ship list's row counts them (the `question` and
   * `manual close` labels). A lane can't take one of these, whatever its place in the lane's list.
   */
  function heldForMaintainer(release) {
    const row = release.ship_list.find((entry) => entry.id === 'maintainer');
    return new Set((row && row.issues) || []);
  }

  /** The issues as a list, by urgency unless they come in an order of their own (@p side, or @p asGiven). */
  function issueList(issues, now, side, asGiven) {
    const sorted = side || asGiven ? issues : issues.slice().sort(byUrgency);
    const held = heldForMaintainer(view.release);
    return el('ul', { class: 'items' }, sorted.map((issue) => {
      const chips = [];
      if (issue.phase !== 'todo') chips.push(chip(PHASE_WORDS[issue.phase], true));
      if (held.has(issue.n)) chips.push(chip('Waiting on the maintainer', true));
      if (issue.sev) chips.push(chip('sev: ' + issue.sev, issue.sev === 'high'));
      if (issue.pri) chips.push(chip('pri: ' + issue.pri, issue.pri === 'high'));
      if (issue.lane !== 'None') chips.push(chip(laneName(issue.lane)));
      chips.push(chip(issue.category));
      for (const pr of issue.pull_requests) chips.push(link('PR #' + pr, '/pull/' + Number(pr), { class: 'chip' }));
      return el('li', null, icon(PHASE_ICONS[issue.phase] || 'todo'),
        el('span', { class: 'item-title' }, issueLink(issue.n), issue.title),
        side ? el('span', { class: 'item-side', text: side(issue) })
          : el('span', { class: 'item-side', title: 'On the milestone since ' + moment(seconds(issue.joined)), text: span((now - seconds(issue.joined)) / HOUR) + ' on milestone' }),
        el('span', { class: 'chips' }, chips));
    }));
  }

  function checklist(release) {
    const states = { done: 'done', review: 'active', progress: 'active', todo: 'todo' };
    return el('div', { class: 'checklist block' }, release.checklist.map((section) => [
      el('h3', { text: section.title || 'Checklist' }),
      el('ul', { class: 'items' }, section.items.map((item) => {
        const chips = [];
        if (item.state === 'review' || item.state === 'progress') chips.push(chip(PHASE_WORDS[item.state], true));
        return el('li', null, icon(states[item.state] || 'todo'), el('span', { class: 'item-title' }, linkify(item.text)), el('span'),
          chips.length ? el('span', { class: 'chips' }, chips) : null);
      })),
    ]));
  }

  /** Text with its #123 references made into links. */
  function linkify(text) {
    return String(text).split(/(#\d+)/).map((piece) => (/^#\d+$/.test(piece) ? link(piece, '/issues/' + Number(piece.slice(1))) : piece));
  }

  function pullRequests(release, now) {
    // This release's first, then the ones that have been ready longest: a pull request sitting for hours is
    // what nobody sees until they open it. Drafts last.
    const since = (pr) => (pr.ready_since ? seconds(pr.ready_since) : Infinity);
    const list = release.live.pull_requests.slice().sort((a, b) => Number(b.in_scope) - Number(a.in_scope) || since(a) - since(b) || a.n - b.n);
    return el('ul', { class: 'items' }, list.map((pr) => {
      const failing = pr.checks === 'FAILURE' || pr.checks === 'ERROR';
      const chips = [chip(pr.draft ? 'Draft' : 'Ready for ' + span((now - since(pr)) / HOUR), !pr.draft)];
      if (pr.waiting && !pr.draft) chips.push(chip('waiting on: ' + pr.waiting, failing));
      if (pr.auto_merge) chips.push(chip('auto-merge on'));
      if (pr.lane !== 'None') chips.push(chip(laneName(pr.lane)));
      if (!pr.in_scope) chips.push(chip(pr.milestone ? pr.milestone : 'not on this milestone'));
      if (pr.base !== release.branch) chips.push(chip('into ' + pr.base));
      for (const n of pr.closes) chips.push(link('closes #' + n, '/issues/' + Number(n), { class: 'chip' }));
      return el('li', null, icon(failing ? 'blocked' : pr.draft ? 'todo' : 'active'),
        el('span', { class: 'item-title' }, link('#' + pr.n, '/pull/' + Number(pr.n), { class: 'item-number' }), pr.title),
        el('span', { class: 'item-side', title: 'Opened ' + moment(seconds(pr.created)), text: 'open ' + span((now - seconds(pr.created)) / HOUR) }),
        el('span', { class: 'chips' }, chips));
    }));
  }

  /** Each watched workflow's latest push run and latest nightly run, the failing ones first. */
  function workflows(release, now) {
    // As main-health.yml judges them: a workflow is red while its latest push run or its latest nightly run failed.
    const passed = (run) => run.conclusion === 'success' || run.conclusion === 'neutral';
    const words = { failure: 'failed', timed_out: 'timed out', startup_failure: "didn't start", action_required: 'needs action' };
    const urgency = (workflow) => (workflow.passing === false ? 0 : workflow.passing == null ? 1 : 2);
    const list = release.live.main.workflows.slice().sort((a, b) => urgency(a) - urgency(b) || a.name.localeCompare(b.name));
    const cell = (workflow, event) => {
      const run = (workflow.runs || []).find((entry) => entry.event === event);
      if (!run) return el('td', { text: '–' });
      const text = (passed(run) ? 'passed' : words[run.conclusion] || run.conclusion) + ' ' + ago(seconds(run.at), now);
      return el('td', { class: passed(run) ? null : 'worse' }, link(text, '/actions/runs/' + Number(run.run)));
    };
    return el('div', { class: 'scroll' }, el('table', { class: 'runs' },
      el('thead', null, el('tr', null, ['Workflow on ' + release.branch, 'Latest push run', 'Latest nightly run'].map((text) => el('th', { scope: 'col', text })))),
      el('tbody', null, list.map((workflow) => el('tr', null,
        el('th', { scope: 'row' }, el('span', { class: 'run-name' }, icon(workflow.passing == null ? 'todo' : workflow.passing ? 'done' : 'blocked'), workflow.name)),
        cell(workflow, 'push'), cell(workflow, 'schedule'))))));
  }

  // ---- movement: the burn-up and the daily bars --------------------------------------------------

  function movement(release, now) {
    const start = seconds(release.cycle.start);
    const end = seconds(release.cycle.end) || now;
    const burn = el('div', { class: 'chart' });
    const daily = el('div', { class: 'chart' });
    const buckets = bucketed(release, start, end);
    view.charts.push(() => drawBurnup(burn, release, start, end), () => drawDaily(daily, buckets, release, start, end));
    const keys = (items) => el('ul', { class: 'legend' }, items.map(([cls, text]) => el('li', null, el('span', { class: 'key ' + cls }), text)));
    // The open work by where it stood: only when the data carries the lists it's drawn from.
    const flows = Boolean(release.events.flow_started && release.events.flow_review);
    const flow = el('div', { class: 'chart' });
    if (flows) view.charts.push(() => drawFlow(flow, release, start, end));
    const flowHead = ['Not started', 'In progress', 'In review'];
    return el('section', null,
      el('div', { class: 'section-head' }, el('h2', { text: 'Scope and progress' }),
        el('p', { class: 'sub', text: 'Since ' + release.cycle.start_reason + ' on ' + day(start) + '. The gap between the two lines is the open work. The charts share one time axis.' })),
      el('div', { class: 'block' }, el('h3', { text: 'Issues on the milestone, and how many are closed' }), keys([['k-scope', 'In scope'], ['k-closed', 'Closed']]), burn),
      el('div', { class: 'block' }, el('h3', { text: 'Closed and added, per ' + buckets.unit }), keys([['box k-closed', 'Closed'], ['box k-scope', 'Added to the milestone']]), daily),
      flows ? el('div', { class: 'block' }, el('h3', { text: 'The open work, by where it stood' }),
        keys([['box k-todo', 'Not started'], ['box k-progress', 'In progress'], ['box k-review', 'In review']]), flow,
        el('p', { class: 'footnote', text: 'Before the `in progress` label (2026-11-19), work shows as started only once it had a pull request. An issue whose pull request was closed unmerged counts as not started again.' })) : null,
      details('table-movement', 'group', el('summary', null, el('span', { text: 'Show the numbers' }), el('span', { text: count(buckets.rows.length, buckets.unit) })),
        el('div', { class: 'scroll' }, el('table', null,
          el('thead', null, el('tr', null, [buckets.unit === 'day' ? 'Day' : 'Week of', 'Closed', 'Added', 'Closed in all', 'In scope', 'Open'].concat(flows ? flowHead : []).map((text) => el('th', { scope: 'col', text })))),
          el('tbody', null, buckets.rows.slice().reverse().map((row) => el('tr', null, el('th', { scope: 'row', text: day(row.from) }),
            el('td', { text: row.closed }), el('td', { text: row.added }), el('td', { text: row.closedTotal }), el('td', { text: row.scope }), el('td', { text: row.scope - row.closedTotal }),
            flows ? [el('td', { text: row.todo }), el('td', { text: row.progress }), el('td', { text: row.review })] : null))))), false));
  }

  /** How the open work stood at @p t: not started, in progress and in review. The lists nest (the generator's flow_events). */
  function standing(release, t) {
    const events = release.events;
    const joined = upTo(events.joined, t);
    const started = Math.min(joined, upTo(events.flow_started || [], t));
    const review = Math.min(started, upTo(events.flow_review || [], t));
    const done = Math.min(review, upTo(events.done, t));
    return { todo: joined - started, progress: started - review, review: review - done };
  }

  /**
   * The time axis the section's three charts share, so a day is at the same place in each: the same plot edges,
   * and whole local days from the cycle's first to today's, or to the goal's while the release is open. The
   * labels sit under the middle of their day, where the bar chart's bars are.
   */
  function timeAxis(release, start, end, width) {
    const left = 44;
    const right = width - 88; // room for the line charts' end labels; the bar chart keeps it too, to line up
    const aim = release.cycle.end ? null : goal(release);
    const first = new Date(start * 1000);
    first.setHours(0, 0, 0, 0);
    const beyond = new Date(Math.max(end, aim ? aim.end : end) * 1000);
    beyond.setHours(24, 0, 0, 0);
    const from = first.getTime() / 1000;
    const to = beyond.getTime() / 1000;
    const x = (t) => left + ((t - from) / Math.max(1, to - from)) * (right - left);
    const middles = [];
    for (const cursor = new Date(first); cursor.getTime() / 1000 < to; cursor.setDate(cursor.getDate() + 1)) {
      const next = new Date(cursor);
      next.setDate(next.getDate() + 1);
      middles.push([(cursor.getTime() + next.getTime()) / 2000, dateFormat.format(cursor)]);
    }
    const every = Math.max(1, Math.ceil(middles.length / Math.max(2, Math.floor((right - left) / 56))));
    const ticks = middles.filter((middle, index) => index % every === 0).map(([t, label]) => [x(t), label]);
    return { left, right, from, to, x, ticks, aim };
  }

  /** How many of the sorted @p times are at or before @p t. */
  function upTo(times, t) {
    let low = 0;
    let high = times.length;
    while (low < high) {
      const middle = (low + high) >> 1;
      if (times[middle] <= t) low = middle + 1;
      else high = middle;
    }
    return low;
  }

  /** The cycle cut into local days, or weeks when it's long, with each one's counts. */
  function bucketed(release, start, end) {
    const joined = release.events.joined;
    const done = release.events.done;
    const first = new Date(start * 1000);
    first.setHours(0, 0, 0, 0);
    const edges = [];
    const cursor = new Date(first);
    while (cursor.getTime() / 1000 < end) {
      edges.push(cursor.getTime() / 1000);
      cursor.setDate(cursor.getDate() + 1);
    }
    edges.push(cursor.getTime() / 1000);
    const step = edges.length - 1 > 45 ? 7 : 1;
    const rows = [];
    for (let index = 0; index + 1 < edges.length; index += step) {
      const from = Math.max(edges[index], start);
      const to = Math.min(edges[Math.min(index + step, edges.length - 1)], end);
      rows.push({
        from: edges[index],
        until: edges[Math.min(index + step, edges.length - 1)],
        closed: upTo(done, to) - upTo(done, from),
        added: upTo(joined, to) - upTo(joined, from),
        closedTotal: upTo(done, to),
        scope: upTo(joined, to),
        ...standing(release, to),
      });
    }
    return { unit: step === 7 ? 'week' : 'day', rows };
  }

  function niceMax(value) {
    if (value <= 5) return 5;
    const power = Math.pow(10, Math.floor(Math.log10(value)));
    for (const multiple of [1, 1.2, 1.6, 2, 2.4, 3, 4, 5, 6, 8, 10]) {
      if (value <= multiple * power) return multiple * power;
    }
    return 10 * power;
  }

  function tooltipFor(container) {
    // Hidden from screen readers: the hit target's label and the "Show the numbers" table carry the same figures,
    // and a live region would announce every pointer move.
    const tip = el('div', { class: 'tooltip', hidden: true, 'aria-hidden': 'true' });
    container.append(tip);
    return {
      show(title, rows, x, y) {
        tip.replaceChildren(el('div', { class: 'tooltip-title', text: title }),
          ...rows.map(([cls, name, value]) => el('div', { class: 'tooltip-row' }, cls ? el('span', { class: 'key ' + cls }) : null, name, el('strong', { text: value }))));
        tip.hidden = false;
        const width = tip.offsetWidth;
        const left = Math.max(0, Math.min(container.clientWidth - width, x + 12 + width > container.clientWidth ? x - width - 12 : x + 12));
        tip.style.left = left + 'px';
        tip.style.top = Math.max(0, y) + 'px';
      },
      hide() {
        tip.hidden = true;
      },
    };
  }

  function axes(chart, box, yMax, xTicks) {
    for (let index = 0; index <= 4; index += 1) {
      const value = (yMax * index) / 4;
      const y = box.bottom - (value / yMax) * (box.bottom - box.top);
      chart.append(svg('line', { class: index === 0 ? 'baseline' : 'grid', x1: box.left, x2: box.right, y1: y, y2: y }),
        svg('text', { x: box.left - 8, y: y + 4, 'text-anchor': 'end', text: number.format(value) }));
    }
    for (const [x, label] of xTicks) chart.append(svg('text', { x, y: box.bottom + 17, 'text-anchor': 'middle', text: label }));
  }

  function drawBurnup(container, release, start, end) {
    const width = Math.max(280, container.clientWidth);
    const height = 250;
    // The axis runs on to the goal while the release is open, so the room left is in the picture.
    const axis = timeAxis(release, start, end, width);
    const box = { left: axis.left, right: axis.right, top: 14, bottom: height - 28 };
    const joined = release.events.joined;
    const done = release.events.done;
    const yMax = niceMax(joined.length);
    const aim = axis.aim;
    const x = axis.x;
    const y = (value) => box.bottom - (value / yMax) * (box.bottom - box.top);

    // A count only changes at an event, so each line is drawn as steps between them.
    const steps = (times) => {
      let total = upTo(times, start);
      let path = 'M' + x(start).toFixed(1) + ' ' + y(total).toFixed(1);
      for (let index = total; index < times.length && times[index] <= end; index += 1) {
        total += 1;
        path += 'H' + x(times[index]).toFixed(1) + 'V' + y(total).toFixed(1);
      }
      return { path: path + 'H' + x(end).toFixed(1), total };
    };
    const scope = steps(joined);
    const closed = steps(done);

    const chart = svg('svg', { viewBox: '0 0 ' + width + ' ' + height, role: 'img',
      'aria-label': 'Burn-up: ' + scope.total + ' issues in scope, ' + closed.total + ' closed, ' + (scope.total - closed.total) + ' open. The table below has the numbers.' });
    axes(chart, box, yMax, axis.ticks);
    const freeze = release.signals.freeze && seconds(release.signals.freeze.at);
    if (freeze && freeze > start && freeze < end) {
      chart.append(svg('line', { class: 'marker', x1: x(freeze), x2: x(freeze), y1: box.top, y2: box.bottom }),
        svg('text', { x: x(freeze) + 5, y: box.top + 10, text: 'Freeze' }));
    }
    if (aim && aim.end > start) {
      const anchor = x(aim.end) > box.right - 40 ? 'end' : 'start';
      chart.append(svg('line', { class: 'marker', x1: x(aim.end), x2: x(aim.end), y1: box.top, y2: box.bottom }),
        svg('text', { x: x(aim.end) + (anchor === 'end' ? -5 : 5), y: box.top + 10, 'text-anchor': anchor, text: 'Goal ' + aim.label }));
    }
    chart.append(svg('path', { class: 'f-closed-wash', d: closed.path + 'V' + box.bottom + 'H' + x(start).toFixed(1) + 'Z' }),
      svg('path', { class: 'line s-scope', d: scope.path }), svg('path', { class: 'line s-closed', d: closed.path }));

    // The end labels sit beside their dots; when the lines meet, the upper one moves up, not away.
    const closedY = y(closed.total);
    const scopeY = Math.min(y(scope.total), closedY - 15);
    const label = (yAt, value, name) => svg('text', { x: x(end) + 10, y: yAt + 4 }, svg('tspan', { class: 'label-strong', text: number.format(value) }), svg('tspan', { text: ' ' + name }));
    chart.append(svg('circle', { class: 'dot f-scope', cx: x(end), cy: y(scope.total), r: 4 }), svg('circle', { class: 'dot f-closed', cx: x(end), cy: closedY, r: 4 }),
      label(scopeY, scope.total, 'in scope'), label(closedY, closed.total, 'closed'));

    const hair = svg('line', { class: 'crosshair', y1: box.top, y2: box.bottom, hidden: true });
    const target = svg('rect', { class: 'hit', x: box.left, y: box.top, width: box.right - box.left, height: box.bottom - box.top, tabindex: 0,
      'aria-label': 'Burn-up chart. Use the left and right arrow keys to read a point in time.' });
    chart.append(hair, target);
    container.replaceChildren(chart);
    const tip = tooltipFor(container);
    let at = end;
    const point = (t) => {
      at = Math.max(start, Math.min(end, t));
      const inScope = upTo(joined, at);
      const isClosed = upTo(done, at);
      hair.setAttribute('x1', x(at));
      hair.setAttribute('x2', x(at));
      hair.removeAttribute('hidden');
      tip.show(moment(at), [['k-scope', 'In scope', number.format(inScope)], ['k-closed', 'Closed', number.format(isClosed)], [null, 'Open', number.format(inScope - isClosed)]], x(at), box.top);
    };
    const leave = () => {
      hair.setAttribute('hidden', '');
      tip.hide();
    };
    target.addEventListener('pointermove', (event) => {
      const bounds = chart.getBoundingClientRect();
      point(axis.from + (((event.clientX - bounds.left) * (width / bounds.width) - box.left) / (box.right - box.left)) * (axis.to - axis.from));
    });
    target.addEventListener('pointerleave', leave);
    target.addEventListener('focus', () => point(at));
    target.addEventListener('blur', leave);
    target.addEventListener('keydown', (event) => {
      const move = { ArrowLeft: -1, ArrowRight: 1 }[event.key];
      if (!move) return;
      event.preventDefault();
      point(at + (move * (end - start)) / 60);
    });
  }

  /** The open work over the cycle as three stacked bands: in review at the bottom, in progress, not started on top. */
  function drawFlow(container, release, start, end) {
    const width = Math.max(280, container.clientWidth);
    const height = 220;
    const axis = timeAxis(release, start, end, width); // the same axis as the two charts above it
    const box = { left: axis.left, right: axis.right, top: 12, bottom: height - 28 };
    const x = axis.x;
    // One sample a pixel is as fine as the screen shows; the counts only change at events.
    const samples = Math.max(2, Math.round(x(end) - x(start)));
    const points = [];
    for (let index = 0; index <= samples; index += 1) {
      const t = start + ((end - start) * index) / samples;
      points.push({ t, ...standing(release, t) });
    }
    const yMax = niceMax(Math.max(1, ...points.map((point) => point.todo + point.progress + point.review)));
    const y = (value) => box.bottom - (value / yMax) * (box.bottom - box.top);

    const now = points[points.length - 1];
    const chart = svg('svg', { viewBox: '0 0 ' + width + ' ' + height, role: 'img',
      'aria-label': 'The open work over the cycle. ' + (release.cycle.end ? 'At the release: ' : 'Now: ') + number.format(now.todo) + ' not started, ' +
        number.format(now.progress) + ' in progress, ' + number.format(now.review) + ' in review. The table below has the numbers.' });
    axes(chart, box, yMax, axis.ticks);

    // Each band lies between two running totals: the area from its lower edge to its upper one.
    const band = (cls, lower, upper) => {
      let path = '';
      points.forEach((point, index) => { path += (index ? 'L' : 'M') + x(point.t).toFixed(1) + ' ' + y(upper(point)).toFixed(1); });
      for (let index = points.length - 1; index >= 0; index -= 1) path += 'L' + x(points[index].t).toFixed(1) + ' ' + y(lower(points[index])).toFixed(1);
      return svg('path', { class: 'area ' + cls, d: path + 'Z' });
    };
    chart.append(
      band('f-review', () => 0, (point) => point.review),
      band('f-progress', (point) => point.review, (point) => point.review + point.progress),
      band('f-todo', (point) => point.review + point.progress, (point) => point.review + point.progress + point.todo));
    const open = now.todo + now.progress + now.review;
    chart.append(svg('circle', { class: 'dot f-todo', cx: x(end), cy: y(open), r: 4 }),
      svg('text', { x: x(end) + 10, y: y(open) + 4 }, svg('tspan', { class: 'label-strong', text: number.format(open) }), svg('tspan', { text: ' open' })));

    const hair = svg('line', { class: 'crosshair', y1: box.top, y2: box.bottom, hidden: true });
    const target = svg('rect', { class: 'hit', x: box.left, y: box.top, width: box.right - box.left, height: box.bottom - box.top, tabindex: 0,
      'aria-label': 'The open work over time. Use the left and right arrow keys to read a point in time.' });
    chart.append(hair, target);
    container.replaceChildren(chart);
    const tip = tooltipFor(container);
    let at = end;
    const point = (t) => {
      at = Math.max(start, Math.min(end, t));
      const stood = standing(release, at);
      hair.setAttribute('x1', x(at));
      hair.setAttribute('x2', x(at));
      hair.removeAttribute('hidden');
      tip.show(moment(at), [['box k-todo', 'Not started', number.format(stood.todo)], ['box k-progress', 'In progress', number.format(stood.progress)],
        ['box k-review', 'In review', number.format(stood.review)], [null, 'Open', number.format(stood.todo + stood.progress + stood.review)]], x(at), box.top);
    };
    const leave = () => {
      hair.setAttribute('hidden', '');
      tip.hide();
    };
    target.addEventListener('pointermove', (event) => {
      const bounds = chart.getBoundingClientRect();
      point(axis.from + (((event.clientX - bounds.left) * (width / bounds.width) - box.left) / (box.right - box.left)) * (axis.to - axis.from));
    });
    target.addEventListener('pointerleave', leave);
    target.addEventListener('focus', () => point(at));
    target.addEventListener('blur', leave);
    target.addEventListener('keydown', (event) => {
      const move = { ArrowLeft: -1, ArrowRight: 1 }[event.key];
      if (!move) return;
      event.preventDefault();
      point(at + (move * (end - start)) / 60);
    });
  }

  function drawDaily(container, buckets, release, start, end) {
    const rows = buckets.rows;
    const width = Math.max(280, container.clientWidth);
    const height = 190;
    const axis = timeAxis(release, start, end, width); // each bucket's bars stand on its own days of the shared axis
    const box = { left: axis.left, right: axis.right, top: 10, bottom: height - 28 };
    const yMax = niceMax(Math.max(1, ...rows.map((row) => Math.max(row.closed, row.added))));
    const y = (value) => box.bottom - (value / yMax) * (box.bottom - box.top);
    const chart = svg('svg', { viewBox: '0 0 ' + width + ' ' + height, role: 'img', 'aria-label': 'Issues closed and added per ' + buckets.unit + '. The table below has the numbers.' });
    axes(chart, box, yMax, axis.ticks);
    container.replaceChildren(chart);
    const tip = tooltipFor(container);

    // A column with a rounded top and a square foot on the baseline.
    const column = (cls, left, value, bar) => {
      if (!value) return null;
      const top = y(value);
      const radius = Math.min(4, bar / 2, box.bottom - top);
      return svg('path', { class: cls, d: 'M' + left + ' ' + box.bottom + 'V' + (top + radius) + 'Q' + left + ' ' + top + ' ' + (left + radius) + ' ' + top +
        'H' + (left + bar - radius) + 'Q' + (left + bar) + ' ' + top + ' ' + (left + bar) + ' ' + (top + radius) + 'V' + box.bottom + 'Z' });
    };
    rows.forEach((row) => {
      const from = axis.x(row.from);
      const band = axis.x(row.until) - from;
      const bar = Math.max(2, Math.min(24, (band - 6) / 2));
      const middle = from + band / 2;
      const text = day(row.from) + ': ' + row.closed + ' closed, ' + row.added + ' added';
      const target = svg('rect', { class: 'hit', x: from, y: box.top, width: band, height: box.bottom - box.top, tabindex: 0, 'aria-label': text });
      const tell = () => tip.show((buckets.unit === 'week' ? 'Week of ' : '') + day(row.from),
        [['box k-closed', 'Closed', row.closed], ['box k-scope', 'Added', row.added], [null, 'Net', signed(row.closed - row.added)]], middle, box.top);
      target.addEventListener('pointerenter', tell);
      target.addEventListener('focus', tell);
      target.addEventListener('pointerleave', tip.hide);
      target.addEventListener('blur', tip.hide);
      chart.append(target);
      for (const mark of [column('f-closed', middle - bar - 1, row.closed, bar), column('f-scope', middle + 1, row.added, bar)]) {
        if (mark) chart.append(mark);
      }
    });
  }

  // ---- open work ---------------------------------------------------------------------------------

  /** One row per lane or kind: a bar of what's open and where it stands, then how fast it's closing. */
  function breakdown(entries, order, heading, label) {
    const rank = (entry) => (order.indexOf(entry.name) + 1) || order.length + 1;
    const sorted = entries.slice().sort((a, b) => rank(a) - rank(b) || a.name.localeCompare(b.name));
    const most = Math.max(1, ...sorted.map((entry) => entry.open));
    return el('div', { class: 'scroll' }, el('table', { class: 'breakdown' },
      el('thead', null,
        el('tr', null, el('th', { scope: 'col', rowspan: 2, text: heading }), el('th', { scope: 'col', rowspan: 2, 'aria-hidden': 'true' }),
          el('th', { scope: 'col', rowspan: 2, text: 'Open' }), el('th', { scope: 'col', rowspan: 2, text: 'Started' }), el('th', { scope: 'colgroup', colspan: 2, text: 'Closed in the last' })),
        el('tr', null, el('th', { scope: 'col', text: '24 h' }), el('th', { scope: 'col', text: '7 days' }))),
      el('tbody', null, sorted.map((entry) => {
        const bar = el('div', { class: 'bar', role: 'img', 'aria-label': entry.todo + ' not started, ' + entry.progress + ' in progress, ' + entry.review + ' in review' });
        bar.style.width = (100 * entry.open) / most + '%';
        for (const phase of ['review', 'progress', 'todo']) {
          if (!entry[phase]) continue;
          const segment = el('span', { class: 'segment k-' + phase, title: PHASE_WORDS[phase] + ': ' + entry[phase] });
          segment.style.flexGrow = entry[phase];
          bar.append(segment);
        }
        return el('tr', null, el('th', { scope: 'row', text: label ? label(entry.name) : entry.name }), el('td', { class: 'bar-cell' }, entry.open ? bar : null),
          el('td', null, el('strong', { text: entry.open })), el('td', { text: entry.progress + entry.review }), el('td', { text: entry.closed_24h }), el('td', { text: entry.closed_7d }));
      }))));
  }

  function groupBy(items, key) {
    const groups = new Map();
    for (const item of items) {
      if (!groups.has(item[key])) groups.set(item[key], []);
      groups.get(item[key]).push(item);
    }
    return groups;
  }

  function bugs(release) {
    const figures = release.figures;
    if (!figures.severities) return null;
    return el('div', { class: 'scroll' }, el('table', null,
      el('thead', null, el('tr', null, ['Severity', 'Found this cycle', 'Fixed', 'Open', 'Opened to fixed, median'].map((text) => el('th', { scope: 'col', text })))),
      el('tbody', null, ['high', 'medium', 'low'].map((severity) => {
        const counts = figures.severities[severity];
        const lead = figures.bug_lead_hours[severity];
        return el('tr', null, el('th', { scope: 'row', text: severity[0].toUpperCase() + severity.slice(1) }), el('td', { text: counts.found }), el('td', { text: counts.fixed }),
          el('td', null, counts.open ? el('strong', { text: counts.open }) : '0'), el('td', { text: lead ? span(lead.median) : '–' }));
      }))));
  }

  function labelGaps(release) {
    const gaps = release.label_gaps || {};
    const kinds = [['no_priority', 'no priority'], ['no_lane', 'no lane'], ['no_severity', 'a bug with no severity']];
    const lines = kinds.filter(([key]) => (gaps[key] || []).length).map(([key, text]) => {
      const numbers = gaps[key];
      return el('li', null, count(numbers.length, 'open issue') + ' with ' + text + ': ', numbers.slice(0, 12).map((n, index) => [index ? ', ' : '', link('#' + n, '/issues/' + Number(n))]),
        numbers.length > 12 ? ' and ' + (numbers.length - 12) + ' more' : '');
    });
    return lines.length ? el('div', { class: 'block' }, el('h3', { text: 'Labels to fix' }), el('ul', { class: 'plain' }, lines)) : null;
  }

  /** Lists the open issues by kind or by lane, and keeps the choice for the next visit. */
  function listBy(key) {
    if (view.listBy === key) return;
    view.listBy = key;
    remember(LIST_BY_KEY, key);
    render();
    // The page was drawn again: the new lists start as the first ones did, with the first open, and the
    // button that was pressed keeps the keyboard's place.
    const first = app.querySelector('.open-lists details');
    if (first) first.open = true;
    const pressed = app.querySelector('.switch button[aria-pressed="true"]');
    if (pressed) pressed.focus();
  }

  function openWork(release, now) {
    const issues = release.open_issues;
    const byCategory = groupBy(issues, 'category');
    const phases = release.counts.phases;
    const legend = el('ul', { class: 'legend' }, [['review', 'In review'], ['progress', 'In progress'], ['todo', 'Not started']].map(([phase, text]) =>
      el('li', null, el('span', { class: 'key box k-' + phase }), text + ' (' + (phases[phase] || 0) + ')')));
    // The lists below, by kind or by lane (#4585). A lane's list runs in the order the lane takes its work.
    const byLane = groupBy(issues, 'lane');
    const groups = view.listBy === 'lane'
      ? ordered(LANES, byLane).map((name) => ({ key: 'open-lane-' + name, name: laneName(name), issues: byLane.get(name).slice().sort(byQueue), asGiven: true }))
      : CATEGORIES.filter((name) => byCategory.has(name)).map((name) => ({ key: 'open-' + name, name, issues: byCategory.get(name), asGiven: false }));
    const switcher = el('div', { class: 'switch', role: 'group', 'aria-label': 'List the open issues by' }, el('span', { 'aria-hidden': 'true', text: 'List by' }),
      LIST_BY.map(([key, text]) => el('button', { type: 'button', 'data-by': key, 'aria-pressed': String(view.listBy === key), onclick: () => listBy(key) }, text)));
    // What has been in hand longest: the first place to look for something stuck.
    const started = issues.filter((issue) => issue.phase !== 'todo' && issue.started).sort((a, b) => seconds(a.started) - seconds(b.started)).slice(0, 5);
    const stuck = started.length ? el('div', { class: 'block' }, el('h3', { text: 'Longest in progress' }),
      issueList(started, now, (issue) => 'started ' + ago(seconds(issue.started), now))) : null;
    const bugTable = bugs(release);
    return el('section', null,
      el('div', { class: 'section-head' }, el('h2', { text: 'Open work' }),
        el('p', { class: 'sub', text: 'Kinds are in the order the lanes take them. An issue is in progress once it carries the `in progress` label or a draft pull request, and in review once a pull request for it is ready.' })),
      legend,
      // Four tables in one ruled grid: a faint line between the columns and between the rows.
      el('div', { class: 'quad' },
        el('div', { class: 'block' }, breakdown(release.by_category || [], CATEGORIES, 'Kind')),
        el('div', { class: 'block' }, breakdown(release.by_lane || [], LANES, 'Lane', laneName)),
        bugTable ? el('div', { class: 'block' }, el('h3', { text: 'Bugs by severity' }), bugTable) : null, stuck),
      labelGaps(release),
      el('div', { class: 'open-lists' }, el('div', { class: 'list-head' }, el('h3', { text: 'Open issues' }), switcher),
        groups.map((group, index) => {
          const begun = group.issues.filter((issue) => issue.phase !== 'todo').length;
          return details(group.key, 'group', el('summary', null, el('span', null, group.name, el('span', { class: 'chevron', 'aria-hidden': 'true' })),
            el('span', { text: count(group.issues.length, 'issue') + (begun ? ', ' + begun + ' started' : '') })), issueList(group.issues, now, null, group.asGiven), index === 0);
        })),
      release.recently_closed.length
        ? details('recent', 'group', el('summary', null, el('span', null, 'Closed in the last 24 hours', el('span', { class: 'chevron', 'aria-hidden': 'true' })), el('span', { text: count(release.recently_closed.length, 'issue') })),
          el('ul', { class: 'items' }, release.recently_closed.map((issue) => el('li', null, icon('done'),
            el('span', { class: 'item-title' }, issueLink(issue.n), issue.title), el('span', { class: 'item-side', text: ago(seconds(issue.at), now) })))), false)
        : null);
  }

  // ---- main's health -----------------------------------------------------------------------------

  function mainHealth(release, now) {
    const start = seconds(release.cycle.start);
    const end = seconds(release.cycle.end) || now;
    const red = release.figures.red;
    const ci = release.figures.ci;
    const strip = el('div', { class: 'strip', role: 'img', 'aria-label': count(red.count, 'red period') + ', ' + span(red.hours) + ' in all, ' + red.percent + '% of the cycle' });
    for (const period of release.red_periods) {
      const from = Math.max(start, seconds(period.start));
      const to = Math.min(end, seconds(period.end) || end);
      if (to <= from) continue;
      const mark = el('a', { class: 'red', href: (repoUrl() || GITHUB) + '/issues/' + Number(period.n),
        title: moment(from) + ', red for ' + span((to - from) / HOUR) + (period.end ? '' : ' so far') + ' (#' + period.n + ')' });
      mark.style.left = (100 * (from - start)) / (end - start) + '%';
      mark.style.width = 'max(3px, ' + (100 * (to - from)) / (end - start) + '%)';
      strip.append(mark);
    }
    // Best pass rate first; a workflow that never ran goes last.
    const rate = (workflow) => (workflow.success + workflow.failure ? workflow.success / (workflow.success + workflow.failure) : -1);
    const percent = (workflow) => (rate(workflow) < 0 ? '–' : (Math.round(1000 * rate(workflow)) / 10) + '%');
    const table = el('table', null,
      el('thead', null, el('tr', null, ['Workflow on ' + release.branch, 'Passed', 'Failed', 'Pass rate'].map((text) => el('th', { scope: 'col', text })))),
      el('tbody', null, ci.workflows.slice().sort((a, b) => rate(b) - rate(a) || b.success - a.success || a.name.localeCompare(b.name)).map((workflow) =>
        el('tr', null, el('th', { scope: 'row', text: workflow.name }), el('td', { text: number.format(workflow.success) }), el('td', { text: number.format(workflow.failure) }),
          el('td', { text: percent(workflow) })))),
      el('tfoot', null, el('tr', null, el('th', { scope: 'row', text: 'All of them' }), el('td', { text: number.format(ci.success) }), el('td', { text: number.format(ci.failure) }),
        el('td', { text: percent(ci) }))));
    return el('section', null,
      el('div', { class: 'section-head' }, el('h2', { text: release.branch + "'s health" }),
        el('p', { class: 'sub' }, release.branch + ' was red ' + count(red.count, 'time') + ' this cycle, for ' + span(red.hours) + ' in all (' + red.percent + '% of the cycle). The longest stretch was ' + span(red.longest_hours) + '.')),
      el('div', { class: 'block' },
        el('ul', { class: 'legend' }, el('li', null, el('span', { class: 'key box k-red' }), 'Red: a watched workflow failed on ' + release.branch)),
        strip, el('div', { class: 'strip-ends' }, el('span', { text: day(start) }), el('span', { text: release.cycle.end ? day(end) : 'now' }))),
      release.live ? el('div', { class: 'block' }, el('h3', { text: 'Latest runs of each watched workflow' }), workflows(release, now)) : null,
      el('div', { class: 'scroll' }, table),
      release.figures.pr_ci && release.figures.pr_ci.pass_percent != null ? el('p', { class: 'sub' }, 'Pull request CI this cycle: ' + number.format(release.figures.pr_ci.success) + ' runs passed and ' +
        number.format(release.figures.pr_ci.failure) + ' failed (' + release.figures.pr_ci.pass_percent + '%)' +
        (release.figures.pr_ci.runs_per_merged != null ? ', ' + release.figures.pr_ci.runs_per_merged + ' runs for each merge (the runs of pull requests still open count too).' : '.')) : null,
      el('p', { class: 'footnote' }, 'Runs on ' + release.branch + ' since the cycle started, counted once an hour; cancelled runs are left out. Coverage is on ',
        el('a', { href: 'https://app.codecov.io/gh/' + (REPO.test(view.index.repo) ? view.index.repo : ''), text: 'Codecov' }), '.'));
  }

  // ---- comparing releases ------------------------------------------------------------------------

  /** The release a release is compared with: the minor before a minor, the one before it on its line for a patch. */
  function predecessor(releases, title) {
    const parts = (text) => text.slice(1).split('.').map(Number);
    const [major, minor, patch] = parts(title);
    const earlier = releases.filter((entry) => {
      const [a, b, c] = parts(entry.title);
      if (patch > 0) return a === major && b === minor && c < patch;
      return c === 0 && (a < major || (a === major && b < minor));
    });
    return earlier.length ? earlier[earlier.length - 1] : null;
  }

  // Each measure: its name, how it reads, its number, and which way is better (none for context).
  const median = (spread) => (spread ? spread.median : null);
  const MEASURES = [
    ['Issues closed per day', (f) => f.closed_per_day ?? '–', (f) => f.closed_per_day, 'up'],
    ['Issue opened to closed, median', (f) => span(median(f.lead_hours)), (f) => median(f.lead_hours), 'down'],
    ['Issue opened to closed, 90th percentile', (f) => span(f.lead_hours ? f.lead_hours.p90 : null), (f) => (f.lead_hours ? f.lead_hours.p90 : null), 'down'],
    ['Wait before work started, median', (f) => span(median(f.queue_hours)), (f) => median(f.queue_hours), 'down'],
    ['Work started to closed, median', (f) => span(median(f.active_hours)), (f) => median(f.active_hours), 'down'],
    ['Pull request opened to merged, median', (f) => span(median(f.merge_hours)), (f) => median(f.merge_hours), 'down'],
    ['Pull request ready to merged, median', (f) => span(median(f.review_hours)), (f) => median(f.review_hours), 'down'],
    ['Pull request CI pass rate', (f) => (f.pr_ci && f.pr_ci.pass_percent != null ? f.pr_ci.pass_percent + '%' : '–'), (f) => (f.pr_ci ? f.pr_ci.pass_percent : null), 'up'],
    // Every pull request's runs over the merges, so pull requests held open through a freeze raise it: context, not judged.
    ['Pull request CI runs per merge', (f) => (f.pr_ci && f.pr_ci.runs_per_merged != null ? String(f.pr_ci.runs_per_merged) : '–'), (f) => (f.pr_ci ? f.pr_ci.runs_per_merged : null), null],
    ['High-severity bug opened to closed, median', (f) => (f.bug_lead_hours.high ? span(f.bug_lead_hours.high.median) + ' (' + f.bug_lead_hours.high.n + ')' : '–'), (f) => median(f.bug_lead_hours.high), 'down'],
    ['Pull request size, median lines', (f) => (f.pull_request_lines ? number.format(Math.round(f.pull_request_lines.median)) : '–'), (f) => median(f.pull_request_lines), 'down'],
    ['Issues reopened', (f) => (f.reopened == null ? '–' : number.format(f.reopened)), (f) => f.reopened ?? null, 'down'],
    ['Share of the cycle red', (f) => f.red.percent + '% (' + span(f.red.hours) + ')', (f) => f.red.percent, 'down'],
    ['Longest red stretch', (f) => span(f.red.longest_hours), (f) => f.red.longest_hours, 'down'],
    ['Branch CI pass rate', (f) => (f.ci.pass_percent == null ? '–' : f.ci.pass_percent + '%'), (f) => f.ci.pass_percent, 'up'],
    ['Cycle length', (f) => f.days + ' d', (f) => f.days, null],
    ['Issues closed', (f) => number.format(f.issues_closed), (f) => f.issues_closed, null],
    ['Carried in from before', (f) => number.format(f.carried_in), (f) => f.carried_in, null],
    ['Added during the cycle', (f) => number.format(f.added_during), (f) => f.added_during, null],
    ['Pull requests merged', (f) => number.format(f.pull_requests_merged), (f) => f.pull_requests_merged, null],
    ['Pull requests closed unmerged', (f) => (f.pull_requests_unmerged == null ? '–' : number.format(f.pull_requests_unmerged)), (f) => f.pull_requests_unmerged ?? null, null],
    ['Bugs fixed: high, medium, low', (f) => (f.severities ? ['high', 'medium', 'low'].map((severity) => f.severities[severity].fixed).join(', ') : '–'), () => null, null],
  ];

  /** How a measure moved from @p before to @p after: gain is the share it improved by (negative: got worse). */
  function movedBy(measure, after, before) {
    const from = measure[2](before);
    const to = measure[2](after);
    if (from == null || to == null) return null;
    const difference = to - from;
    const relative = from !== 0 ? difference / Math.abs(from) : difference === 0 ? 0 : Math.sign(difference) * Infinity;
    return { difference, relative, gain: measure[3] === 'up' ? relative : measure[3] === 'down' ? -relative : null };
  }

  function changeCell(change, judged) {
    if (!change) return el('td', { text: '–' });
    const size = Math.abs(change.relative);
    const share = !isFinite(size) ? 'from 0' : (size < 0.1 ? Math.round(size * 1000) / 10 : Math.round(size * 100)) + '%';
    if (!judged) {
      const rounded = Math.round(change.difference * 10) / 10;
      return el('td', { class: 'context', text: rounded === 0 ? 'same' : signed(rounded) + (isFinite(size) ? ' (' + share + ')' : '') });
    }
    if (change.gain === 0) return el('td', { text: 'no change' });
    const better = change.gain > 0;
    return el('td', { class: better ? 'better' : 'worse' }, el('span', { 'aria-hidden': 'true', text: better ? '▲ ' : '▼ ' }), share + (better ? ' better' : ' worse'));
  }

  function compare(release) {
    const releases = view.index.releases.filter((entry) => entry.stage !== 'planned');
    const title = release.milestone.title;
    const current = releases.find((entry) => entry.title === title);
    const before = current ? predecessor(releases, title) : null;
    let rows = MEASURES.map((measure) => ({ measure, change: current && before ? movedBy(measure, current.figures, before.figures) : null }));
    if (before) {
      // Best improvement first, worst last; then the measures with no better or worse, for context.
      const judged = rows.filter((row) => row.measure[3] && row.change);
      const unjudged = rows.filter((row) => !(row.measure[3] && row.change));
      judged.sort((a, b) => b.change.gain - a.change.gain);
      rows = judged.concat(unjudged);
    }
    const firstContext = rows.findIndex((row) => !(row.measure[3] && row.change));
    const columns = releases.length + (before ? 2 : 1);
    const partial = releases.filter((entry) => entry.figures.partial);
    // The release being read, then how it changed, then the others going back in time: on a phone the
    // first three columns are what fits.
    const others = releases.filter((entry) => entry.title !== title).reverse();
    const heading = (entry) => entry.title + (entry.figures.partial ? ' †' : '') + (entry.stage === 'released' ? '' : ' so far');
    const body = [];
    rows.forEach((row, index) => {
      if (before && index === firstContext) body.push(el('tr', { class: 'divider' }, el('th', { scope: 'rowgroup', colspan: columns, text: 'For context: neither better nor worse' })));
      body.push(el('tr', null, el('th', { scope: 'row', text: row.measure[0] }),
        current ? el('td', { class: 'now', text: row.measure[1](current.figures) }) : null,
        before ? changeCell(row.change, Boolean(row.measure[3])) : null,
        others.map((entry) => el('td', { text: row.measure[1](entry.figures) }))));
    });
    return el('section', null,
      el('div', { class: 'section-head' }, el('h2', { text: 'Release over release' }),
        el('p', { class: 'sub', text: before
          ? title + ' against ' + before.title + ', the biggest improvement first and the biggest step back last.' + (current.stage === 'released' ? '' : ' ' + title + " isn't finished, so its figures will still move.")
          : 'Each cycle runs from the release before it to its own.' })),
      el('div', { class: 'scroll' }, el('table', { class: 'compare' },
        el('thead', null, el('tr', null, el('th', { scope: 'col', text: 'Measure' }),
          current ? el('th', { scope: 'col', class: 'now', text: heading(current) }) : null,
          before ? el('th', { scope: 'col', text: 'Change from ' + before.title }) : null,
          others.map((entry) => el('th', { scope: 'col', text: heading(entry) })))),
        el('tbody', null, body))),
      el('p', { class: 'footnote', text: 'Each cycle runs from the release before it to its own. "Work started" is when the first linked pull request opened, for every release. Medians, so a few issues carried for months don\'t decide a figure.' }),
      partial.length ? el('p', { class: 'footnote', text: '† ' + partial.map((entry) => entry.title + "'s milestone was opened " + entry.figures.tracked_days + ' days before it shipped, so earlier work in that cycle is not counted').join('; ') + '.' }) : null);
  }

  function footer(release) {
    return el('footer', null,
      el('p', { class: 'footnote' }, 'Read from the milestone, the release tracker, the labels and the Actions runs: nothing is kept anywhere else. Generated ' + moment(seconds(release.generated_at)) + ' by ',
        link('tools/release-dashboard.py', '/blob/main/tools/release-dashboard.py'), ', every 10 minutes.'),
      el('p', { class: 'footnote' }, link('How the page reads the tracker and the labels', '/blob/main/docs/dev/release-process.md'), ' · ',
        link('All milestones', '/milestones'), ' · ', link('Actions', '/actions')));
  }

  // ---- go ----------------------------------------------------------------------------------------

  let resizing = 0;
  new ResizeObserver(() => {
    if (!view.release || app.clientWidth === view.width) return;
    cancelAnimationFrame(resizing);
    resizing = requestAnimationFrame(() => {
      view.width = app.clientWidth;
      for (const draw of view.charts) draw();
    });
  }).observe(app);

  setInterval(() => {
    if (document.visibilityState === 'visible' && !window.__RELEASE_DATA__) show(null, true);
  }, REFRESH_MS);
  setInterval(() => {
    if (view.clock) view.clock();
  }, 60 * 1000);

  show(null);
})();
