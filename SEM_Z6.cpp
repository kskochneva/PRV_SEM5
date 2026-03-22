#include <iostream>
#include <thread>
#include <queue>
#include <vector>
#include <map>
#include <mutex>
#include <chrono>
#include <atomic>
#include <random>
#include <condition_variable>

using namespace std::chrono_literals;

// Структура части файла
struct FileChunk {
    int chunk_id;
    int file_id;
    size_t size;           // Размер в байтах (для симуляции)

    void download() {
        // Симуляция загрузки: время зависит от размера
        std::this_thread::sleep_for(std::chrono::milliseconds(size / 100));
    }
};

// Структура файла
struct FileDownload {
    int file_id;
    std::string filename;
    std::vector<FileChunk> chunks;
    std::atomic<int> downloaded_chunks{0};
    std::chrono::steady_clock::time_point startTime;

    bool is_complete() const {
        return downloaded_chunks.load() == (int)chunks.size();
    }

    void mark_chunk_downloaded() {
        downloaded_chunks++;
    }

    int getProgress() const {
        return (downloaded_chunks.load() * 100) / chunks.size();
    }
};

class DownloadManager {
private:
    std::queue<FileChunk> chunkQueue;           // Очередь частей на загрузку
    std::mutex queueMutex;
    std::condition_variable cv;

    std::counting_semaphore<> activeDownloads;  // Ограничение количества файлов
    std::counting_semaphore<> chunkDownloads;   // Ограничение количества частей

    std::map<int, FileDownload> files;          // Хранилище файлов
    std::mutex filesMutex;

    std::atomic<int> completedFiles{0};
    std::atomic<long long> totalDownloadedBytes{0};
    std::mutex coutMutex;

    int maxConcurrentFiles;
    int maxConcurrentChunks;

public:
    DownloadManager(int max_files, int max_chunks)
        : activeDownloads(max_files)
        , chunkDownloads(max_chunks)
        , maxConcurrentFiles(max_files)
        , maxConcurrentChunks(max_chunks) {}

    // Добавление файла в систему
    void add_file(FileDownload file) {
        file.startTime = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(filesMutex);
            files[file.file_id] = file;

            std::lock_guard<std::mutex> coutLock(coutMutex);
            std::cout << "[ADD] Файл #" << file.file_id
                      << " (" << file.filename << ") добавлен, частей: "
                      << file.chunks.size() << "\n";
        }

        // Добавляем все части файла в очередь
        for (const auto& chunk : file.chunks) {
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                chunkQueue.push(chunk);
            }
            cv.notify_one();
        }
    }

    // Поток-загрузчик
    void download_worker(int workerId) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> yieldChance(1, 100);

        while (true) {
            FileChunk chunk;
            bool hasChunk = false;

            // Извлекаем часть из очереди
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                cv.wait(lock, [this] { return !chunkQueue.empty(); });
                chunk = chunkQueue.front();
                chunkQueue.pop();
                hasChunk = true;
            }

            if (!hasChunk) continue;

            // Захватываем слот для части
            chunkDownloads.acquire();

            // Захватываем слот для файла (если это первая часть файла)
            bool needFileSlot = false;
            {
                std::lock_guard<std::mutex> lock(filesMutex);
                auto it = files.find(chunk.file_id);
                if (it != files.end() && it->second.downloaded_chunks.load() == 0) {
                    needFileSlot = true;
                }
            }

            if (needFileSlot) {
                activeDownloads.acquire();
                {
                    std::lock_guard<std::mutex> coutLock(coutMutex);
                    std::cout << "[WORKER " << workerId << "] Начата загрузка файла #"
                              << chunk.file_id << "\n";
                }
            }

            // Скачиваем часть
            {
                std::lock_guard<std::mutex> coutLock(coutMutex);
                std::cout << "[WORKER " << workerId << "] Поток "
                          << std::this_thread::get_id()
                          << " скачивает файл #" << chunk.file_id
                          << ", часть " << chunk.chunk_id
                          << " (размер=" << chunk.size << " байт)\n";
            }

            // Симуляция загрузки с возможностью уступки CPU
            auto startChunk = std::chrono::steady_clock::now();
            while (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startChunk).count() < chunk.size / 100) {

                std::this_thread::sleep_for(50ms);

                if (yieldChance(gen) > 95) {
                    std::this_thread::yield();
                }
            }

            totalDownloadedBytes += chunk.size;

            // Обновляем статус файла
            bool fileCompleted = false;
            {
                std::lock_guard<std::mutex> lock(filesMutex);
                auto it = files.find(chunk.file_id);
                if (it != files.end()) {
                    it->second.mark_chunk_downloaded();

                    if (it->second.is_complete()) {
                        fileCompleted = true;
                        completedFiles++;

                        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - it->second.startTime);

                        std::lock_guard<std::mutex> coutLock(coutMutex);
                        std::cout << "\n[COMPLETE] Файл #" << chunk.file_id
                                  << " (" << it->second.filename << ") полностью скачан!\n";
                        std::cout << "  Время загрузки: " << duration.count() << " мс\n";
                        std::cout << "  Прогресс: 100%\n\n";
                    } else {
                        int progress = it->second.getProgress();
                        if (progress % 20 == 0) {  // Выводим прогресс каждые 20%
                            std::lock_guard<std::mutex> coutLock(coutMutex);
                            std::cout << "[PROGRESS] Файл #" << chunk.file_id
                                      << " (" << it->second.filename << "): "
                                      << progress << "%\n";
                        }
                    }
                }
            }

            // Освобождаем слот для части
            chunkDownloads.release();

            // Если файл завершен, освобождаем слот для файла
            if (fileCompleted) {
                activeDownloads.release();
            }

            // Небольшая задержка между загрузками
            std::this_thread::sleep_for(50ms);
            std::this_thread::yield();
        }
    }

    int getCompletedFiles() const { return completedFiles.load(); }
    long long getTotalDownloadedBytes() const { return totalDownloadedBytes.load(); }
};

// Создание тестового файла с частями
FileDownload createTestFile(int fileId, const std::string& name, int numChunks, size_t chunkSize) {
    FileDownload file;
    file.file_id = fileId;
    file.filename = name;

    for (int i = 0; i < numChunks; ++i) {
        FileChunk chunk;
        chunk.chunk_id = i;
        chunk.file_id = fileId;
        chunk.size = chunkSize;
        file.chunks.push_back(chunk);
    }

    return file;
}

int main() {
    const int MAX_CONCURRENT_FILES = 2;   // Одновременно не более 2 файлов
    const int MAX_CONCURRENT_CHUNKS = 4;  // Одновременно не более 4 частей
    const int NUM_WORKERS = 3;            // 3 потока-загрузчика

    DownloadManager manager(MAX_CONCURRENT_FILES, MAX_CONCURRENT_CHUNKS);
    std::vector<std::thread> workers;

    std::cout << "=== МЕНЕДЖЕР ЗАГРУЗОК ===\n";
    std::cout << "Макс. одновременно файлов: " << MAX_CONCURRENT_FILES << "\n";
    std::cout << "Макс. одновременно частей: " << MAX_CONCURRENT_CHUNKS << "\n";
    std::cout << "Потоков-загрузчиков: " << NUM_WORKERS << "\n\n";

    auto startTime = std::chrono::high_resolution_clock::now();

    // Запускаем потоки-загрузчики
    for (int i = 1; i <= NUM_WORKERS; ++i) {
        workers.emplace_back(&DownloadManager::download_worker, &manager, i);
        workers.back().detach();
    }

    // Создаем тестовые файлы
    std::vector<FileDownload> testFiles;
    testFiles.push_back(createTestFile(1, "movie_1080p.mp4", 10, 1024));     // 10 частей по 1KB
    testFiles.push_back(createTestFile(2, "document.pdf", 5, 512));          // 5 частей по 512B
    testFiles.push_back(createTestFile(3, "archive.zip", 8, 768));           // 8 частей по 768B
    testFiles.push_back(createTestFile(4, "setup.exe", 6, 1024));            // 6 частей по 1KB
    testFiles.push_back(createTestFile(5, "music.mp3", 4, 256));             // 4 части по 256B

    // Добавляем файлы в менеджер
    for (auto& file : testFiles) {
        manager.add_file(file);
        std::this_thread::sleep_for(500ms);  // Пауза между добавлением файлов
    }

    // Ждем завершения всех загрузок
    std::this_thread::sleep_for(15s);

    auto endTime = std::chrono::high_resolution_clock::now();
    auto totalTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

    std::cout << "\n========== СТАТИСТИКА ЗАГРУЗОК ==========\n";
    std::cout << "Завершено файлов: " << manager.getCompletedFiles() << "/" << testFiles.size() << "\n";
    std::cout << "Всего загружено: " << manager.getTotalDownloadedBytes() << " байт\n";
    std::cout << "Общее время: " << totalTime.count() << " мс\n";
    std::cout << "=========================================\n";

    return 0;
}
