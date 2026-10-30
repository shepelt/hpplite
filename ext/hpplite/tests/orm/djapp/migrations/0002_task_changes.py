# The migrations an app accumulates: each of these rebuilds the table on SQLite
from django.db import migrations, models

def fill_priority(apps, schema_editor):
    Task = apps.get_model("djapp", "Task")
    Task.objects.filter(points__gte=3).update(priority="high")

class Migration(migrations.Migration):
    dependencies = [("djapp", "0001_initial")]
    operations = [
        migrations.AlterField(model_name="task", name="title", field=models.CharField(max_length=200)),
        migrations.RemoveField(model_name="task", name="legacy"),
        migrations.AddField(model_name="task", name="priority", field=models.CharField(default="normal", max_length=10)),
        migrations.RunPython(fill_priority, migrations.RunPython.noop),
    ]
