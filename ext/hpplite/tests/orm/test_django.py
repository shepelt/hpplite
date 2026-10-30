"""Django ORM and migrations on HPPLite: see orm_common.py."""
import datetime, os, sys
from orm_common import uri, step, probe, verify, finish
import django
from django.conf import settings

FILE = "django.db"
settings.configure(
    DATABASES={"default": {"ENGINE": "django.db.backends.sqlite3", "NAME": uri(FILE)}},
    INSTALLED_APPS=["django.contrib.contenttypes", "django.contrib.auth", "djapp"],
    USE_TZ=True, DEFAULT_AUTO_FIELD="django.db.models.BigAutoField",
    PASSWORD_HASHERS=["django.contrib.auth.hashers.MD5PasswordHasher"])   # fast; hashing is Python-side anyway
django.setup()
from django.core.management import call_command
from django.db import connection, transaction, IntegrityError
from django.db.models import F, Q, Count, Sum, Value
from django.db.models.functions import Now, Concat, Upper, Random
from django.contrib.auth.models import User, Group, Permission
from djapp.models import Project, Tag, Task

print(f"== Django {django.__version__}")

def migrate():
    # auth + contenttypes + this app: dozens of table rebuilds, then post_migrate bulk_create of permissions
    call_command("migrate", verbosity=0)
    assert Permission.objects.count() > 0

def seed():
    tags = Tag.objects.bulk_create([Tag(name=n) for n in ("bug", "feature", "chore")])
    for i in range(4):
        u = User.objects.create_user(f"user{i}", f"u{i}@example.com", "pw")
        for j in range(3):
            p = Project.objects.create(owner=u, title=f"P{i}.{j}", slug=f"p-{i}-{j}", meta={"n": j})
            ts = Task.objects.bulk_create([Task(project=p, assignee=u, title=f"T{k}", points=k,
                                                due=datetime.date(2026, 1 + k, 15)) for k in range(5)])
            for t in ts:
                t.tags.add(tags[t.points % 3])
    g = Group.objects.create(name="editors")
    g.permissions.add(*Permission.objects.filter(codename__startswith="change_"))
    User.objects.get(username="user0").groups.add(g)

def f_expressions():
    Task.objects.filter(points__gte=2).update(points=F("points") * 10, title=Concat(F("title"), Value("!")))
    Project.objects.filter(title__startswith="P1").update(title=Upper("title"))

def get_or_create():
    Tag.objects.get_or_create(name="bug")
    Tag.objects.get_or_create(name="new")
    Tag.objects.update_or_create(name="new", defaults={"name": "newer"})

def bulk_update():
    ts = list(Task.objects.filter(project__slug="p-2-0"))
    for t in ts:
        t.done = True; t.points += 1
    Task.objects.bulk_update(ts, ["done", "points"])          # UPDATE ... SET x = CASE WHEN id=.. THEN ..

def atomic_nested():
    with transaction.atomic():
        Tag.objects.create(name="outer")
        try:
            with transaction.atomic():
                Tag.objects.create(name="inner")
                Tag.objects.create(name="outer")              # unique violation -> savepoint rollback
        except IntegrityError:
            pass
        Tag.objects.create(name="after")
    names = set(Tag.objects.values_list("name", flat=True))
    assert {"outer", "after"} <= names and "inner" not in names, names

def select_for_update():
    with transaction.atomic():
        t = Task.objects.select_for_update().filter(done=False).first()
        t.done = True; t.save()

def save_and_auto_now():
    p = Project.objects.get(slug="p-0-0")
    p.meta["edited"] = True; p.save()                          # auto_now: a Python datetime, bound
    p.refresh_from_db(); assert p.meta["edited"]

def python_cascade():
    # Django's on_delete runs in Python: collects and deletes children, SET_NULL included
    User.objects.get(username="user3").delete()
    assert not Project.objects.filter(owner__username="user3").exists()

def m2m_and_aggregate():
    t = Task.objects.first()
    t.tags.set(Tag.objects.filter(name__in=["bug", "chore"]))
    list(Project.objects.annotate(n=Count("tasks"), s=Sum("tasks__points")).filter(n__gt=0))

def date_lookup_update():
    # __year on a DateField is a BETWEEN on literals: deterministic
    Task.objects.filter(due__year=2026, points__lt=5).update(done=True)

step("migrate: auth, contenttypes, app (table rebuilds, RunPython)", migrate)
step("seed: create_user, bulk_create, M2M add, permissions", seed)
step("QuerySet.update with F(), Concat, Upper", f_expressions)
step("get_or_create / update_or_create", get_or_create)
step("bulk_update (CASE WHEN)", bulk_update)
step("nested atomic with an IntegrityError in a savepoint", atomic_nested)
step("select_for_update in atomic", select_for_update)
step("save() with auto_now and JSONField", save_and_auto_now)
step("delete with Python-side CASCADE and SET_NULL", python_cascade)
step("M2M set, annotate/aggregate reads", m2m_and_aggregate)
step("update filtered by __year", date_lookup_update)

probe("update filtered by __month (django_date_extract, a Python UDF)",
      lambda: Task.objects.filter(due__month=3).update(points=F("points") + 1))
probe("update with Now()", lambda: Task.objects.filter(pk=1).update(title=Concat(F("title"), Now())))
probe("order_by('?') read (RAND, a Python UDF)", lambda: list(Tag.objects.order_by("?")))
probe("order_by(Random()) read", lambda: list(Tag.objects.order_by(Random())))
probe("last_login update via update_last_login (Python timezone.now())",
      lambda: User.objects.filter(username="user0").update(last_login=datetime.datetime.now(datetime.timezone.utc)))

connection.close()
verify(FILE)
finish("django")
