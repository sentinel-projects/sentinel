#include "sentinel/core/job/BackgroundJobService.h"
#include <QSqlDatabase>
#include <QtTest>

using namespace sentinel::core;

class BackgroundJobPersistenceTest final : public QObject {
    Q_OBJECT
private slots:
    void persistsAndRecoversTerminalMetadata();
};

void BackgroundJobPersistenceTest::persistsAndRecoversTerminalMetadata() {
    const QString connection = QStringLiteral("job-persistence");
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
    database.setDatabaseName(QStringLiteral(":memory:"));
    QVERIFY(database.open());
    BackgroundJobService service;
    QVERIFY(service.setPersistenceDatabase(database));
    const QString id = service.submitJob(
        QStringLiteral("success"), [](Job&, std::function<void(JobProgress)>) { return true; });
    QTRY_VERIFY_WITH_TIMEOUT(service.findJob(id).has_value(), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(service.findJob(id)->state, JobState::Completed, 5000);
    database.close();
    QSqlDatabase::removeDatabase(connection);
}

QTEST_MAIN(BackgroundJobPersistenceTest)
#include "test_background_job_persistence.moc"
