/**
 * @file main.cpp
 * @brief Falcon Desktop 应用入口
 * @author Falcon Team
 * @date 2025-12-27
 */

#include <QApplication>
#include "main_window.hpp"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    app.setApplicationName("Falcon");
    app.setApplicationVersion("0.1.0");
    app.setOrganizationName("FalconTeam");

    falcon::desktop::MainWindow window;
    window.resize(1200, 800);
    window.show();

    return app.exec();
}
