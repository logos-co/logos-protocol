#include "implementations/qt_remote_plain/qtro_transport.h"

#include <QCoreApplication>
#include <QLocalServer>

#include <cstdlib>
#include <iostream>

int main(int argc, char** argv)
{
    // A service-launched daemon may have no TMPDIR. Compare against the real
    // Qt local server in that environment, in a fresh process so Qt has not
    // cached its temporary-directory choice from another test.
    ::unsetenv("TMPDIR");
    QCoreApplication application(argc, argv);
    const std::string name = "lq_" + std::to_string(QCoreApplication::applicationPid());
    QLocalServer server;
    if (!server.listen(QString::fromStdString(name))) return 1;
    const std::string actual = server.fullServerName().toStdString();
    const std::string plain = logos::qt_remote_plain::localSocketPath("local:" + name);
    if (plain != actual) {
        std::cerr << "plain: " << plain << "\nQt: " << actual << '\n';
        return 2;
    }
    return 0;
}
