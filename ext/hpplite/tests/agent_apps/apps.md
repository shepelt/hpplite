# App briefs

Each agent got the common brief (BRIEF_COMMON.md), its working directory, a development
environment (a fresh database and a fresh funded wallet per run, on a local chain), and one
of these. Nothing else: no HPPLite source, no tests, no hints about what HPPLite refuses.

| App | Stack | Brief |
|-----|-------|-------|
| tasks | Python: FastAPI + SQLAlchemy 2 ORM + Alembic | Team task tracker: signup/login (hashed passwords, tokens), projects with members, tasks (assignee, status transitions, due dates), comments, an activity log; deleting a project removes its tasks; filters; a 1000-task bulk import in one request. v2 (Alembic, on existing data): task labels (many-to-many) and a priority column with a default. |
| booking | Python: Django ORM + migrations | Restaurant table booking: restaurants, tables, opening hours, customers, bookings with conflict checks and party-size validation, cancellation, availability search, daily occupancy report; ~300 bookings with rejected double bookings. v2 (Django migration): a waitlist and booking notes. |
| shortener | Python: Flask + stdlib sqlite3, hand-written SQL and migration | URL shortener: generated codes, 302 redirects recording each click (time, referrer, agent), per-link stats, API keys, delete with clicks; ~200 links, ~3000 clicks in bursts. v2: custom aliases and expiry (410). |
| inventory | Node: Express + Knex + Knex migrations | Shop inventory and orders: products, stock movements, customers, orders with line items placed in one transaction that checks and decrements stock, cancellation restores stock, low-stock and sales-by-day reports; a 500-product import in one transaction, ~1000 orders. v2 (Knex migration): suppliers and a product supplier reference. |
| forum | Node: Express/Hono/Fastify + Drizzle ORM + migrations | Forum: users, categories, threads, posts, one vote per user per post, karma, activity ordering, title search, moderator deletion; a seed of 50 users, 300 threads, 3000 posts in one go. v2: post edit history and an edited marker. |
| habits | Node: node:http + node:sqlite, no framework, no ORM | Habit tracker: habits with schedules, one check-in per habit per day with undo, current and longest streaks, weekly summaries, archive and delete; ~90 days of backfilled check-ins (thousands, some in batch requests). v2: reminders and per-user time zones. |
