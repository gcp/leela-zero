/*
    This file is part of Leela Zero.
    Copyright (C) 2017 Gian-Carlo Pascutto

    Leela Zero is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Leela Zero is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Leela Zero.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <QtCore/QCoreApplication>
#include <QtCore/QTimer>
#include <QtCore/QTextStream>
#include <QtCore/QStringList>
#include <QtCore/QPair>
#include <QtCore/QVector>
#include <QProcess>
#include <QFile>
#include <QDir>
#include <QRegularExpression>
#include <QUuid>
#include <QDebug>
#include <chrono>
#include <iostream>
#include "Game.h"

constexpr int AUTOGTP_VERSION = 2;

bool fetch_best_network_hash(QTextStream& cerr, QString& nethash, long games_played) {
    QString prog_cmdline("curl");
#ifdef WIN32
    prog_cmdline.append(".exe");
#endif
    prog_cmdline.append(" http://zero.sjeng.org/best-network-hash");
    QProcess curl;
    curl.start(prog_cmdline);
    curl.waitForFinished(-1);
    QByteArray output = curl.readAllStandardOutput();
    QString outstr(output);
    QStringList outlst = outstr.split("\n");
    if (outlst.size() != 2) {
        cerr << "Unexpected output from server: " << endl << output << endl;
        exit(EXIT_FAILURE);
    }
    QString outhash = outlst[0];
    QString client_version = outlst[1];
    auto server_expected = client_version.toInt();
    if (server_expected > AUTOGTP_VERSION) {
        cerr << "Server requires client version " << server_expected
             << " but we are version " << AUTOGTP_VERSION << endl;
        cerr << "Check https://github.com/gcp/leela-zero for updates." << endl;
        exit(EXIT_FAILURE);
    }
    if (games_played == 0) {
        cerr << "Best network hash: " << outhash << endl;
        cerr << "Required client version: " << server_expected << " (OK)" << endl;
    }
    nethash = outhash;
    return true;
}

bool fetch_best_network(QTextStream& cerr, QString& netname, long games_played) {
    if (QFileInfo::exists(netname)) {
        if (games_played == 0) {
            cerr << "Already downloaded network." << endl;
        }
        return true;
    }

    QString prog_cmdline("curl");
#ifdef WIN32
    prog_cmdline.append(".exe");
#endif
    // Be quiet, but output the real file name we saved to
    // Use the filename from the server
    // Resume download if file exists (aka avoid redownloading, and don't
    // error out if it exists)
    prog_cmdline.append(" -s -O -J");
    prog_cmdline.append(" -w %{filename_effective}");
    prog_cmdline.append(" http://zero.sjeng.org/best-network");

    cerr << prog_cmdline << endl;

    QProcess curl;
    curl.start(prog_cmdline);
    curl.waitForFinished(-1);

    QByteArray output = curl.readAllStandardOutput();
    QString outstr(output);
    QStringList outlst = outstr.split("\n");
    QString outfile = outlst[0];
    cerr << "Curl filename: " << outfile << endl;
#ifdef WIN32
    QProcess::execute("gunzip.exe -k -q " + outfile);
#else
    QProcess::execute("gunzip -k -q " + outfile);
#endif
    // Remove extension (.gz)
    outfile.chop(3);
    cerr << "Net filename: " << outfile << endl;
    netname = outfile;

    return true;
}

bool upload_data(QTextStream& cerr, const QString& netname) {
    // Find output SGF and txt files
    QDir dir;
    QStringList filters;
    filters << "*.sgf";
    dir.setNameFilters(filters);
    dir.setFilter(QDir::Files | QDir::NoSymLinks);

    QFileInfoList list = dir.entryInfoList();
    for (int i = 0; i < list.size(); ++i) {
        QFileInfo fileInfo = list.at(i);
        QString sgf_file = fileInfo.fileName();
        QString data_file = sgf_file;
        // Cut .sgf, add .txt.0.gz
        data_file.chop(4);
        data_file += ".txt.0.gz";
        // Gzip up the sgf too
#ifdef WIN32
        QProcess::execute("gzip.exe " + sgf_file);
#else
        QProcess::execute("gzip " + sgf_file);
#endif
        sgf_file += ".gz";
        QString prog_cmdline("curl");
#ifdef WIN32
        prog_cmdline.append(".exe");
#endif
        prog_cmdline.append(" -F networkhash=" + netname);
        prog_cmdline.append(" -F clientversion=" + QString::number(AUTOGTP_VERSION));
        prog_cmdline.append(" -F sgf=@" + sgf_file);
        prog_cmdline.append(" -F trainingdata=@" + data_file);
        prog_cmdline.append(" http://zero.sjeng.org/submit");
        cerr << prog_cmdline << endl;
        QProcess curl;
        curl.start(prog_cmdline);
        curl.waitForFinished(-1);
        QByteArray output = curl.readAllStandardOutput();
        QString outstr(output);
        cerr << outstr;
        dir.remove(sgf_file);
        dir.remove(data_file);
    }
    return true;
}

bool run_one_game(QTextStream& cerr, const QString& weightsname) {

    Game game(weightsname, cerr);
    if(!game.gameStart()) {
        return false;
    }
    do {
        game.move();
        if(!game.waitForMove()) {
            return false;
        }
        game.readMove();
    } while (game.nextMove());
    cerr << "Game has ended." << endl;
    if (game.getScore()) {
        game.writeSgf();
        game.dumpTraining();
    }
    cerr << "Stopping engine." << endl;
    game.gameQuit();
    return true;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTimer::singleShot(0, &app, SLOT(quit()));

    // Map streams
    QTextStream cin(stdin, QIODevice::ReadOnly);
    QTextStream cout(stdout, QIODevice::WriteOnly);
#if defined(LOG_ERRORS_TO_FILE)
    // Log stderr to file
    QFile caFile("output.txt");
    caFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append);
    if(!caFile.isOpen()){
        qDebug() << "- Error, unable to open" << "outputFilename" << "for output";
    }
    QTextStream cerr(&caFile);
#else
    QTextStream cerr(stderr, QIODevice::WriteOnly);
#endif
    cerr << "autogtp v" << AUTOGTP_VERSION << endl;

    auto start = std::chrono::high_resolution_clock::now();
    auto success = true;
    long games_played = 0;

    do {
        auto game_start = std::chrono::high_resolution_clock::now();
        QString netname;
        success &= fetch_best_network_hash(cerr, netname, games_played);
        success &= fetch_best_network(cerr, netname, games_played);
        success &= run_one_game(cerr, netname);
        success &= upload_data(cerr, netname);
        games_played++;

        auto game_end = std::chrono::high_resolution_clock::now();
        auto game_time_s = std::chrono::duration_cast<std::chrono::seconds>(game_end - game_start);
        auto total_time_s = std::chrono::duration_cast<std::chrono::seconds>(game_end - start);
        cerr << games_played << " games_played in "
             << (total_time_s.count() / 60) << " minutes = "
             << (total_time_s.count() / games_played) << " seconds/game, last game "
             << game_time_s.count() << " seconds\n\n";
    } while (success);

    cerr.flush();
    cout.flush();
    return app.exec();
}
