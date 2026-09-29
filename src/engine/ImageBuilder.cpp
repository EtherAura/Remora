#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "core/Builders.h"
#include "core/ImageBuild.h"
#include "engine/Engine.h"

namespace remora {

// Run argv on the build host: directly when local, wrapped over ssh when rc.buildHost is set.
static ProcResult runOnBuildHost(Spawner &sp, const std::optional<QString> &host,
                                 const QStringList &argv, const LineSink &log) {
    if (!host || host->isEmpty()) return sp.run(argv, {}, log);
    QStringList quoted;
    for (QString a : argv) {
        if (a.contains(QLatin1Char(' ')) || a.contains(QLatin1Char('"'))) {
            a.replace(QLatin1Char('\''), QLatin1String("'\\''"));
            a = QLatin1Char('\'') + a + QLatin1Char('\'');
        }
        quoted << a;
    }
    return sp.run(Spawner::sshArgv(*host, quoted.join(QLatin1Char(' '))), {}, log);
}

static LineSink mkLog(LogSink &sink, const QString &step) {
    return [&sink, step](const QString &stream, const QString &text) {
        sink.line(step, stream, text);
    };
}

QVector<Step> imageBuildSteps(Spawner &sp, const ResolvedConfig &rc, const ImageRecipe &recipe) {
    QVector<Step> steps;
    const bool remote = rc.buildHost && !rc.buildHost->isEmpty();

    // Recipe build: stage the context (Dockerfile + operator-staged payloads), build, export.
    steps << Step{QStringLiteral("stage"), QStringLiteral("Stage build context"),
                  [&sp, &rc, recipe, remote](LogSink &sink) {
                      if (recipe.needsSourceBuild)
                          return StepResult::fail(recipe.sourceBuildHint);
                      if (!recipe.needsBuild)
                          return StepResult::good(
                              QStringLiteral("base image %1 already satisfies the features")
                                  .arg(recipe.baseTag));
                      const LineSink log = mkLog(sink, QStringLiteral("stage"));
                      if (remote) {
                          auto r = sp.run(Spawner::sshArgv(*rc.buildHost,
                                                           QStringLiteral("mkdir -p %1 && cat > "
                                                                          "%1/Dockerfile <<'EOF'\n%2EOF")
                                                               .arg(rc.buildDir, recipe.dockerfile)),
                                          {}, log);
                          if (r.rc != 0)
                              return StepResult::fail(QStringLiteral("staging Dockerfile on %1 failed")
                                                          .arg(*rc.buildHost),
                                                      r.stderrTail);
                          for (const QString &p : recipe.payloads) {
                              auto t = sp.run(Spawner::sshArgv(*rc.buildHost,
                                                               QStringLiteral("test -e %1/%2")
                                                                   .arg(rc.buildDir, p)),
                                              {}, log);
                              if (t.rc != 0)
                                  return StepResult::fail(
                                      QStringLiteral("payload missing on %1: %2/%3 — stage it first")
                                          .arg(*rc.buildHost, rc.buildDir, p));
                          }
                      } else {
                          if (!QDir().mkpath(rc.buildDir))
                              return StepResult::fail(
                                  QStringLiteral("cannot create build dir %1").arg(rc.buildDir));
                          QFile f(rc.buildDir + QStringLiteral("/Dockerfile"));
                          if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                              return StepResult::fail(
                                  QStringLiteral("cannot write %1").arg(f.fileName()));
                          f.write(recipe.dockerfile.toUtf8());
                          f.close();
                          for (const QString &p : recipe.payloads)
                              if (!QFileInfo::exists(rc.buildDir + QLatin1Char('/') + p))
                                  return StepResult::fail(
                                      QStringLiteral("payload missing: %1/%2 — stage it first")
                                          .arg(rc.buildDir, p));
                      }
                      return StepResult::good(QStringLiteral("context ready in %1").arg(rc.buildDir));
                  }};
    steps << Step{QStringLiteral("build"), QStringLiteral("docker build"),
                  [&sp, &rc, recipe](LogSink &sink) {
                      if (!recipe.needsBuild)
                          return StepResult::good(QStringLiteral("skipped — no overlay needed"));
                      auto r = runOnBuildHost(sp, rc.buildHost,
                                              buildImageBuildArgv(recipe.tag, rc.buildDir),
                                              mkLog(sink, QStringLiteral("build")));
                      if (r.rc != 0)
                          return StepResult::fail(QStringLiteral("docker build failed"),
                                                  r.stderrTail);
                      return StepResult::good(recipe.tag);
                  }};
    steps << Step{QStringLiteral("export"), QStringLiteral("Export image archive"),
                  [&sp, &rc, recipe, remote](LogSink &sink) {
                      if (!recipe.needsBuild)
                          return StepResult::good(QStringLiteral("skipped — no overlay needed"));
                      const LineSink log = mkLog(sink, QStringLiteral("export"));
                      const QString archive = imageArchivePath(rc.outputDir, recipe.tag);
                      if (remote) {
                          auto m = sp.run(Spawner::sshArgv(*rc.buildHost,
                                                           QStringLiteral("mkdir -p %1")
                                                               .arg(rc.outputDir)),
                                          {}, log);
                          if (m.rc != 0)
                              return StepResult::fail(QStringLiteral("cannot create %1 on %2")
                                                          .arg(rc.outputDir, *rc.buildHost),
                                                      m.stderrTail);
                      } else if (!QDir().mkpath(rc.outputDir)) {
                          return StepResult::fail(
                              QStringLiteral("cannot create output dir %1").arg(rc.outputDir));
                      }
                      auto r = runOnBuildHost(sp, rc.buildHost,
                                              buildImageSaveArgv(recipe.tag, archive), log);
                      if (r.rc != 0)
                          return StepResult::fail(QStringLiteral("docker save failed"),
                                                  r.stderrTail);
                      return StepResult::good(archive);
                  }};
    return steps;
}

}  // namespace remora
