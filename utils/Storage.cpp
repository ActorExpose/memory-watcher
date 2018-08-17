/*
  Memory watcher
  Copyright (C) 2018 Lukas Karas <karas@avast.com>

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "Storage.h"

#include <QDebug>
#include <QSqlError>
#include <QSqlQuery>

#include "QVariantConverters.h"

Storage::Storage()
{
}

Storage::~Storage()
{
  if (db.isValid()) {
    if (db.isOpen()) {
      db.close();
    }
    db = QSqlDatabase(); // invalidate instance
    QSqlDatabase::removeDatabase("storage");
  }
  qDebug() << "Storage is closed";
}

bool Storage::updateSchema()
{
  QStringList tables = db.tables();

  if (!tables.contains("range")) {

    QString sql("CREATE TABLE `range`");
    sql.append("(").append( "`id` INTEGER PRIMARY KEY");
    sql.append(",").append( "`from` INTEGER NOT NULL ");
    sql.append(",").append( "`to` INTEGER NOT NULL ");
    sql.append(",").append( "`permission` varchar(255) NULL ");
    sql.append(",").append( "`name` varchar(255) NULL ");
    sql.append(");");

    QSqlQuery q = db.exec(sql);
    if (q.lastError().isValid()) {
      qWarning() << "Creating table range failed" << q.lastError();
      db.close();
      return false;
    }
  }

  if (!tables.contains("measurement")) {

    QString sql("CREATE TABLE `measurement`");
    sql.append("(").append( "`id` INTEGER NOT NULL PRIMARY KEY");
    sql.append(",").append( "`time` datetime NOT NULL");
    sql.append(",").append( "`rss_sum` INTEGER NOT NULL ");
    sql.append(",").append( "`pss_sum` INTEGER NOT NULL ");
    sql.append(");");

    QSqlQuery q = db.exec(sql);
    if (q.lastError().isValid()) {
      qWarning() << "Creating measurement table failed" << q.lastError();
      db.close();
      return false;
    }
  }

  if (!tables.contains("data")) {

    QString sql("CREATE TABLE `data`");
    sql.append("(").append( "`range_id` INTEGER NOT NULL REFERENCES range(id) ON DELETE CASCADE");
    sql.append(",").append( "`measurement_id` INTEGER NOT NULL REFERENCES measurement(id) ON DELETE CASCADE");
    sql.append(",").append( "`rss` INTEGER NOT NULL ");
    sql.append(",").append( "`pss` INTEGER NOT NULL ");
    sql.append(");");

    QSqlQuery q = db.exec(sql);
    if (q.lastError().isValid()) {
      qWarning() << "Creating data table failed" << q.lastError();
      db.close();
      return false;
    }
  }

  return true;
}

bool Storage::init(QString file)
{
  // Find QSLite driver
  db = QSqlDatabase::addDatabase("QSQLITE", "storage");
  if (!db.isValid()){
    qWarning() << "Could not find QSQLITE backend";
    return false;
  }

  db.setDatabaseName(file);
  if (!db.open()){
    qWarning() << "Open database failed" << db.lastError();
    return false;
  }
  qDebug() << "Storage database opened:" << file;
  if (!updateSchema()){
    return false;
  }

  QSqlQuery q = db.exec("PRAGMA foreign_keys = ON;");
  if (q.lastError().isValid()){
    qWarning() << "Enabling foreign keys fails:" << q.lastError();
  }

  return db.isValid() && db.isOpen();
}

qlonglong Storage::insertRange(qlonglong from, qlonglong to, QString permission, QString name)
{
  QSqlQuery sql(db);
  sql.prepare("INSERT INTO `range` (`from`, `to`, `permission`, `name`) VALUES (:from, :to, :permission, :name)");
  sql.bindValue(":from", from);
  sql.bindValue(":to", to);
  sql.bindValue(":permission", permission);
  sql.bindValue(":name", name);

  sql.exec();
  if (sql.lastError().isValid()) {
    qWarning() << "Insert range failed" << sql.lastError();
    return 0;
  }

  return converters::varToLong(sql.lastInsertId());
}

qlonglong Storage::insertMeasurement(const QDateTime &time, qlonglong rss, qlonglong pss)
{
  QSqlQuery sql(db);
  sql.prepare("INSERT INTO `measurement` (`time`, `rss_sum`, `pss_sum`) VALUES (:time, :rss, :pss)");
  db.transaction();

  sql.bindValue(":time", time);
  sql.bindValue(":rss", rss);
  sql.bindValue(":pss", pss);

  sql.exec();
  if (sql.lastError().isValid()) {
    qWarning() << "Insert measurement failed" << sql.lastError();
    return 0;
  }

  return converters::varToLong(sql.lastInsertId());
}

bool Storage::insertData(qlonglong measurementId, const std::vector<MeasurementData> &measurements)
{
  QSqlQuery sql(db);
  sql.prepare("INSERT INTO `data` (`range_id`, `measurement_id`, `rss`, `pss`) VALUES (:range_id, :measurement_id, :rss, :pss)");
  db.transaction();
  for (const auto &m: measurements) {
    sql.bindValue(":range_id", m.rangeId);
    sql.bindValue(":measurement_id", measurementId);
    sql.bindValue(":rss", m.rss);
    sql.bindValue(":pss", m.pss);

    sql.exec();
    if (sql.lastError().isValid()) {
      qWarning() << "Insert data failed" << sql.lastError();
      return false;
    }
  }

  return true;
}

bool Storage::getMeasurement(Measurement &measurement, QSqlQuery &measurementQuery)
{
  measurementQuery.exec();
  if (measurementQuery.lastError().isValid()) {
    qWarning() << "Select measurement failed" << measurementQuery.lastError();
    return false;
  }
  if (!measurementQuery.next()){
    qWarning() << "No measurement found";
    return false;
  }
  measurement.id = converters::varToLong(measurementQuery.value("id"));
  measurement.time = converters::varToDateTime(measurementQuery.value("time"));

  QSqlQuery sql(db);

  // ranges
  sql.prepare("SELECT * FROM `range` WHERE `id` IN (SELECT `range_id` FROM `data` WHERE `measurement_id` = :measurement_id);");
  sql.bindValue(":measurement_id", measurement.id);
  sql.exec();
  if (sql.lastError().isValid()) {
    qWarning() << "Select ranges failed" << sql.lastError();
    return false;
  }
  measurement.rangeMap.clear();
  while (sql.next()){
    Range range;
    range.from = converters::varToLong(sql.value("from"));
    range.to = converters::varToLong(sql.value("to"));
    range.permission = converters::varToString(sql.value("permission"));
    range.name = converters::varToString(sql.value("name"));
    measurement.rangeMap[converters::varToLong(sql.value("id"))] = range;
  }

  // data
  sql.prepare("SELECT * FROM `data` WHERE `measurement_id` = :measurement_id");
  sql.bindValue(":measurement_id", measurement.id);
  sql.exec();
  if (sql.lastError().isValid()) {
    qWarning() << "Select ranges failed" << sql.lastError();
    return false;
  }
  measurement.data.clear();
  while (sql.next()) {
    MeasurementData data;
    data.rangeId = converters::varToLong(sql.value("range_id"));;
    data.rss = converters::varToLong(sql.value("rss"));
    data.pss = converters::varToLong(sql.value("pss"));
    measurement.data << data;
  }

  return true;
}

bool Storage::getMemoryPeak(Measurement &measurement, MemoryType type)
{
  QSqlQuery sql(db);

  // measurement
  if (type == Rss) {
    sql.prepare("SELECT * FROM `measurement` WHERE rss_sum = (SELECT MAX(rss_sum) FROM `measurement`) LIMIT 1;");
  }else{
    sql.prepare("SELECT * FROM `measurement` WHERE pss_sum = (SELECT MAX(pss_sum) FROM `measurement`) LIMIT 1;");
  }
  return getMeasurement(measurement, sql);
}
