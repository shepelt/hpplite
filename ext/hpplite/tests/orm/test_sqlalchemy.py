"""SQLAlchemy 2.x (Core + ORM) and Alembic on HPPLite: see orm_common.py."""
import datetime, sqlite3
from orm_common import uri, step, probe, verify, finish
from sqlalchemy import (create_engine, event, ForeignKey, String, Integer, JSON, DateTime, Table, Column,
                        select, insert, update, delete, func, text, MetaData)
from sqlalchemy.dialects.sqlite import insert as sqlite_insert
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column, relationship, Session
from alembic.migration import MigrationContext
from alembic.operations import Operations

FILE = "sqlalchemy.db"
engine = create_engine("sqlite://", creator=lambda: sqlite3.connect(uri(FILE), uri=True, check_same_thread=False))

@event.listens_for(engine, "connect")
def _fk_on(dbapi_conn, _):          # the recipe in SQLAlchemy's SQLite docs
    dbapi_conn.execute("PRAGMA foreign_keys=ON")

class Base(DeclarativeBase):
    pass

task_tags = Table("task_tags", Base.metadata,
                  Column("task_id", ForeignKey("tasks.id", ondelete="CASCADE"), primary_key=True),
                  Column("tag_id", ForeignKey("tags.id", ondelete="CASCADE"), primary_key=True))

class User(Base):
    __tablename__ = "users"
    id: Mapped[int] = mapped_column(primary_key=True)
    email: Mapped[str] = mapped_column(String(200), unique=True)
    name: Mapped[str]
    created_at: Mapped[datetime.datetime] = mapped_column(default=lambda: datetime.datetime(2026, 1, 1))
    projects: Mapped[list["Project"]] = relationship(back_populates="owner", cascade="all, delete-orphan")

class Project(Base):
    __tablename__ = "projects"
    id: Mapped[int] = mapped_column(primary_key=True)
    owner_id: Mapped[int] = mapped_column(ForeignKey("users.id", ondelete="CASCADE"))
    title: Mapped[str]
    settings: Mapped[dict] = mapped_column(JSON, default=dict)
    owner: Mapped[User] = relationship(back_populates="projects")
    tasks: Mapped[list["Task"]] = relationship(back_populates="project", passive_deletes=True)

class Task(Base):
    __tablename__ = "tasks"
    id: Mapped[int] = mapped_column(primary_key=True)
    project_id: Mapped[int] = mapped_column(ForeignKey("projects.id", ondelete="CASCADE"), index=True)
    title: Mapped[str]
    done: Mapped[bool] = mapped_column(default=False)
    points: Mapped[int] = mapped_column(default=1)
    project: Mapped[Project] = relationship(back_populates="tasks")
    tags: Mapped[list["Tag"]] = relationship(secondary=task_tags)

class Tag(Base):
    __tablename__ = "tags"
    id: Mapped[int] = mapped_column(primary_key=True)
    name: Mapped[str] = mapped_column(unique=True)

class Comment(Base):
    __tablename__ = "comments"
    id: Mapped[int] = mapped_column(primary_key=True)
    task_id: Mapped[int] = mapped_column(ForeignKey("tasks.id", ondelete="CASCADE"))
    parent_id: Mapped[int | None] = mapped_column(ForeignKey("comments.id", ondelete="CASCADE"))
    body: Mapped[str]

print("== SQLAlchemy + Alembic")

def create():
    Base.metadata.create_all(engine)

def seed():
    with Session(engine) as s, s.begin():
        tags = [Tag(name=n) for n in ("bug", "feature", "chore")]
        for i in range(5):
            u = User(email=f"u{i}@example.com", name=f"User {i}")
            for j in range(3):
                p = Project(title=f"P{i}.{j}", settings={"color": "blue", "n": j})
                p.tasks = [Task(title=f"T{i}.{j}.{k}", points=k, tags=[tags[k % 3]]) for k in range(4)]
                u.projects.append(p)
            s.add(u)

def bulk_returning():
    # insertmanyvalues: one INSERT ... VALUES (...), (...) RETURNING id per page
    with Session(engine) as s, s.begin():
        pid = s.scalar(select(Project.id).limit(1))
        ids = s.scalars(insert(Task).returning(Task.id), [{"project_id": pid, "title": f"bulk {i}"} for i in range(250)]).all()
        assert len(ids) == 250

def updates():
    with Session(engine) as s, s.begin():
        s.execute(update(Task).where(Task.points >= 2).values(done=True, points=Task.points * 10))
        t = s.scalar(select(Task).where(Task.title == "T0.0.1"))
        t.title = "renamed"; t.tags.append(s.scalar(select(Tag).where(Tag.name == "chore")))
        p = s.scalar(select(Project).where(Project.title == "P1.1"))
        p.settings = {**p.settings, "archived": True}

def upsert():
    with engine.begin() as c:
        stmt = sqlite_insert(Tag).values([{"id": 1, "name": "bug"}, {"id": 99, "name": "urgent"}])
        c.execute(stmt.on_conflict_do_update(index_elements=["id"], set_={"name": stmt.excluded.name + "!"}))

def savepoints():
    with Session(engine) as s, s.begin():
        s.add(Tag(name="kept"))
        nested = s.begin_nested()
        s.add(Tag(name="rolled-back"))
        s.flush()
        nested.rollback()
        with s.begin_nested():
            s.add(Tag(name="kept-too"))
    with Session(engine) as s:
        names = set(s.scalars(select(Tag.name)))
        assert "kept" in names and "kept-too" in names and "rolled-back" not in names, names

def rollback():
    try:
        with Session(engine) as s, s.begin():
            s.add(Tag(name="never"))
            s.flush()
            raise RuntimeError("app error")
    except RuntimeError:
        pass
    with Session(engine) as s:
        assert s.scalar(select(func.count()).where(Tag.name == "never")) == 0

def comments():
    with Session(engine) as s, s.begin():
        tid = s.scalar(select(Task.id).limit(1))
        root = Comment(task_id=tid, body="root"); s.add(root); s.flush()
        reply = Comment(task_id=tid, parent_id=root.id, body="reply"); s.add(reply); s.flush()
        s.add(Comment(task_id=tid, parent_id=reply.id, body="reply to reply"))

def db_cascade():
    # ON DELETE CASCADE in the database (passive_deletes): tasks, task_tags, comments go too
    with Session(engine) as s, s.begin():
        before = s.scalar(select(func.count()).select_from(Task))
        s.execute(delete(Project).where(Project.title == "P0.0"))
    with Session(engine) as s:
        assert s.scalar(select(func.count()).select_from(Task)) < before
        assert s.scalar(select(func.count()).select_from(Comment)) == 0

def orm_cascade():
    with Session(engine) as s, s.begin():
        s.delete(s.scalar(select(User).where(User.email == "u4@example.com")))

def alembic_batch():
    # the move-and-copy migration SQLite needs for most ALTERs
    with engine.begin() as c:
        op = Operations(MigrationContext.configure(c))
        op.add_column("users", Column("bio", String, server_default=""))
        op.create_index("ix_tasks_done", "tasks", ["done"])
        with op.batch_alter_table("comments", recreate="always") as b:
            b.add_column(Column("edited", Integer, nullable=False, server_default="0"))
            b.alter_column("body", type_=String(500))
        op.execute("UPDATE users SET bio = 'hello ' || name")

def with_pragmas():
    with engine.begin() as c:
        c.exec_driver_sql("PRAGMA foreign_keys")          # read
        c.exec_driver_sql("PRAGMA table_info(users)")

step("create_all (tables, unique, indexes, FKs with ON DELETE CASCADE)", create)
step("ORM seed: relationships, many-to-many, JSON column", seed)
step("bulk insert with RETURNING (insertmanyvalues)", bulk_returning)
step("UPDATE with expressions, attribute edits, JSON edit", updates)
step("upsert: INSERT ... ON CONFLICT DO UPDATE", upsert)
step("savepoints: begin_nested rollback and commit", savepoints)
step("transaction rollback on app error", rollback)
step("self-referential FK rows", comments)
step("database-level ON DELETE CASCADE", db_cascade)
step("ORM-level cascade delete", orm_cascade)
step("self-referential FK rows, again (the cascade took the first ones)", comments)
step("Alembic: add column, index, batch table rebuild", alembic_batch)
step("PRAGMA reads", with_pragmas)

def p_server_default_now():
    t = Table("audit", MetaData(), Column("id", Integer, primary_key=True),
              Column("at", DateTime, server_default=func.now()))
    t.create(engine)
def p_func_now():
    with engine.begin() as c:
        c.execute(update(User).values(name=User.name).where(User.id == func.abs(1)))
        c.execute(text("UPDATE users SET bio = bio || datetime('now') WHERE id = 1"))
def p_order_random():
    with Session(engine) as s:
        s.scalars(select(Tag).order_by(func.random())).all()
def p_wal():
    with engine.connect() as c:
        c.exec_driver_sql("PRAGMA journal_mode=WAL").fetchall()
probe("server_default=func.now() (DEFAULT CURRENT_TIMESTAMP)", p_server_default_now)
probe("UPDATE ... datetime('now')", p_func_now)
probe("SELECT ... ORDER BY random() (read only)", p_order_random)
probe("PRAGMA journal_mode=WAL", p_wal)

engine.dispose()
verify(FILE)
finish("sqlalchemy")
