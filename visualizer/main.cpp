#include <QApplication>
#include <QVBoxLayout>
#include <QWidget>
#include <QPushButton>
#include <QFileDialog>
#include "AudioVisualizer.h"

// IMPORTANT: Include your existing QRhiWidget wrapper here!
// Assuming you have something like this in your project:
// #include "../src/ShaderArt.h" 

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    QWidget window;
    QVBoxLayout *layout = new QVBoxLayout(&window);

    AudioVisualizer visualizer;

    // --- PSEUDO CODE: Substitute this with your actual QRhiWidget ---
    // ShaderArt *shaderWidget = new ShaderArt(); 
    // layout->addWidget(shaderWidget);
    //
    // QObject::connect(&visualizer, &AudioVisualizer::amplitudeChanged, shaderWidget, [shaderWidget](float amp) {
    //     // Inside your ShaderArt's rendering loop (or QRhi update loop):
    //     // Update the uniform buffer!
    //     // ubufData.amplitude = amp;
    //     // shaderWidget->update(); 
    // });
    // ----------------------------------------------------------------

    QPushButton *btnPlay = new QPushButton("Select Audio & Play");
    layout->addWidget(btnPlay);

    QObject::connect(btnPlay, &QPushButton::clicked, [&visualizer]() {
        QString file = QFileDialog::getOpenFileName(nullptr, "Open Audio", "", "Audio Files (*.mp3 *.ogg *.flac *.wav)");
        if (!file.isEmpty()) {
            visualizer.play(QUrl::fromLocalFile(file));
        }
    });

    window.resize(600, 400);
    window.show();

    return app.exec();
}