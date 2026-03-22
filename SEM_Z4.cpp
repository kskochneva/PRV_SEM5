#include <iostream>
#include <thread>
#include <queue>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>
#include <string>
#include <random>

using namespace std::chrono_literals;

// Структура задания на печать
struct PrintJob {
    int id;
    std::string document;
    int priority;        // 1 - высший, 5 - низший
    int duration_ms;     // время печати в миллисекундах
    std::chrono::steady_clock::time_point startTime;

    // Оператор сравнения для priority_queue (чем меньше priority, тем выше приоритет)
    bool operator<(const PrintJob& other) const {
        return priority > other.priority;  // Меньшее число = выше приоритет
    }
};

class PrinterQueue {
private:
    int numPrinters;
    std::counting_semaphore<> printerSemaphore;  // Ограничиваем количество принтеров
    std::priority_queue<PrintJob> jobQueue;      // Очередь с приоритетами
    std::mutex queueMutex;
    std::condition_variable cv;
    std::atomic<int> completedJobs{0};
    std::atomic<int> interruptedJobs{0};
    std::mutex coutMutex;

    // Активные задания (для отслеживания прерываний)
    struct ActiveJob {
        PrintJob job;
        std::thread::id threadId;
        bool shouldInterrupt = false;
        std::condition_variable interruptCV;
        std::mutex interruptMutex;
    };
    std::vector<ActiveJob> activeJobs;
    std::mutex activeJobsMutex;

public:
    PrinterQueue(int printers) : numPrinters(printers), printerSemaphore(printers) {}

    // Добавление задания в очередь
    void printJob(const std::string& doc, int priority, int timeout_ms) {
        static int nextId = 1;
        PrintJob job;
        job.id = nextId++;
        job.document = doc;
        job.priority = priority;
        job.duration_ms = timeout_ms;

        {
            std::lock_guard<std::mutex> lock(queueMutex);
            jobQueue.push(job);
            {
                std::lock_guard<std::mutex> coutLock(coutMutex);
                std::cout << "[QUEUE] Задание #" << job.id
                          << " (приор=" << priority << ", док='" << doc
                          << "') добавлено в очередь\n";
            }
        }
        cv.notify_one();
    }

    // Работа принтера (выполняется в отдельном потоке)
    void printerWorker(int printerId) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> interruptCheck(1, 10);

        while (true) {
            // Захватываем семафор (ждем свободный принтер)
            printerSemaphore.acquire();

            PrintJob job;
            bool hasJob = false;

            // Извлекаем задание из очереди
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                cv.wait(lock, [this] { return !jobQueue.empty(); });
                job = jobQueue.top();
                jobQueue.pop();
                hasJob = true;
                job.startTime = std::chrono::steady_clock::now();

                {
                    std::lock_guard<std::mutex> coutLock(coutMutex);
                    std::cout << "[PRINTER " << printerId << "] Поток "
                              << std::this_thread::get_id()
                              << " начал печать задания #" << job.id
                              << " (приор=" << job.priority << ")\n";
                }
            }

            // Регистрируем активное задание
            ActiveJob active;
            active.job = job;
            active.threadId = std::this_thread::get_id();
            {
                std::lock_guard<std::mutex> lock(activeJobsMutex);
                activeJobs.push_back(active);
            }

            // Имитация печати с возможностью прерывания
            bool interrupted = false;
            auto startPrint = std::chrono::steady_clock::now();
            int elapsed = 0;

            while (elapsed < job.duration_ms) {
                // Проверяем, не нужно ли прервать задание
                bool shouldInterrupt = false;
                {
                    std::lock_guard<std::mutex> lock(activeJobsMutex);
                    for (auto& aj : activeJobs) {
                        if (aj.threadId == std::this_thread::get_id() && aj.shouldInterrupt) {
                            shouldInterrupt = true;
                            break;
                        }
                    }
                }

                if (shouldInterrupt) {
                    interrupted = true;
                    interruptedJobs++;
                    {
                        std::lock_guard<std::mutex> coutLock(coutMutex);
                        std::cout << "[PRINTER " << printerId << "] Поток "
                                  << std::this_thread::get_id()
                                  << " ПРЕРВАЛ печать задания #" << job.id << "\n";
                    }
                    break;
                }

                // Печатаем порциями по 100 мс
                std::this_thread::sleep_for(100ms);
                elapsed += 100;

                // Выводим прогресс (каждую секунду)
                if (elapsed % 1000 == 0 && elapsed < job.duration_ms) {
                    {
                        std::lock_guard<std::mutex> coutLock(coutMutex);
                        std::cout << "[PRINTER " << printerId << "] Задание #" << job.id
                                  << " печатается... " << elapsed/1000 << "/"
                                  << job.duration_ms/1000 << " сек\n";
                    }
                }

                // Уступаем процессор
                std::this_thread::yield();
            }

            if (!interrupted) {
                completedJobs++;
                {
                    std::lock_guard<std::mutex> coutLock(coutMutex);
                    std::cout << "[PRINTER " << printerId << "] Поток "
                              << std::this_thread::get_id()
                              << " завершил печать задания #" << job.id
                              << " (документ: " << job.document << ")\n";
                }
            } else {
                // Если прервано, возвращаем задание в очередь (если нужно)
                // Но по условию "прерываемые задания" - просто отменяем
                // Можно было бы вернуть в очередь, но для простоты просто отменяем
            }

            // Удаляем из активных заданий
            {
                std::lock_guard<std::mutex> lock(activeJobsMutex);
                for (auto it = activeJobs.begin(); it != activeJobs.end(); ++it) {
                    if (it->threadId == std::this_thread::get_id()) {
                        activeJobs.erase(it);
                        break;
                    }
                }
            }

            // Освобождаем семафор (принтер свободен)
            printerSemaphore.release();

            std::this_thread::sleep_for(100ms);
            std::this_thread::yield();
        }
    }

    // Прерывание задания по ID
    bool interruptJob(int jobId) {
        std::lock_guard<std::mutex> lock(activeJobsMutex);
        for (auto& aj : activeJobs) {
            if (aj.job.id == jobId) {
                aj.shouldInterrupt = true;
                {
                    std::lock_guard<std::mutex> coutLock(coutMutex);
                    std::cout << "[SYSTEM] Отправлен запрос на прерывание задания #"
                              << jobId << "\n";
                }
                return true;
            }
        }
        return false;
    }

    int getCompletedJobs() const { return completedJobs.load(); }
    int getInterruptedJobs() const { return interruptedJobs.load(); }
};

// Функция для демонстрации работы
int main() {
    PrinterQueue printerQueue(2);  // 2 принтера

    std::vector<std::thread> printers;

    // Запускаем потоки принтеров
    for (int i = 1; i <= 2; ++i) {
        printers.emplace_back(&PrinterQueue::printerWorker, &printerQueue, i);
    }

    // Немного ждем для инициализации
    std::this_thread::sleep_for(500ms);

    // Добавляем задания с разным приоритетом
    printerQueue.printJob("Отчет_2024.pdf", 3, 3000);   // приор 3, 3 сек
    printerQueue.printJob("Контракт_клиент.docx", 1, 2000); // приор 1, 2 сек (срочный!)
    printerQueue.printJob("Презентация.pptx", 4, 4000); // приор 4, 4 сек
    printerQueue.printJob("Срочное_письмо.txt", 1, 1500);   // приор 1, 1.5 сек (срочный!)
    printerQueue.printJob("Инвойс_123.pdf", 2, 2500);   // приор 2, 2.5 сек

    std::this_thread::sleep_for(3s);

    // Прерываем долгое задание
    printerQueue.interruptJob(3);  // Презентацию прерываем

    // Даем время на завершение
    std::this_thread::sleep_for(5s);

    // Выводим статистику
    std::cout << "\n========== СТАТИСТИКА ПЕЧАТИ ==========\n";
    std::cout << "Завершено заданий: " << printerQueue.getCompletedJobs() << "\n";
    std::cout << "Прервано заданий: " << printerQueue.getInterruptedJobs() << "\n";
    std::cout << "=======================================\n";

    // В реальном приложении нужно корректно завершить потоки
    // Здесь для простоты программа завершается

    return 0;
}
