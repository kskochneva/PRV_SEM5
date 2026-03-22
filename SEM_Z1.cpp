#include <iostream>
#include <thread>
#include <semaphore>
#include <mutex>
#include <queue>
#include <vector>
#include <chrono>
#include <condition_variable>
#include <atomic>

using namespace std::chrono_literals;

// Структура для представления ожидающего потока с приоритетом
struct WaitingThread {
    int priority;
    std::condition_variable cv; // Каждый поток ждет на своей CV
    bool granted = false;       // Флаг, получил ли поток доступ
};

class ResourcePool {
private:
    std::counting_semaphore<> semaphore;
    std::mutex mtx;
    // Очередь с приоритетом. std::greater<> делает так, что наименьшее число (1 - высокий приоритет) вверху.
    std::priority_queue<std::pair<int, std::shared_ptr<WaitingThread>>,
                        std::vector<std::pair<int, std::shared_ptr<WaitingThread>>>,
                        std::greater<>> waitingQueue;
    std::atomic<int> failed_attempts{0};

public:
    ResourcePool(int count) : semaphore(count) {}

    // Поток пытается получить ресурс
    void acquire(int priority, int timeout_ms) {
        auto thread_id = std::this_thread::get_id();

        // 1. Пытаемся сразу захватить семафор (получить ресурс)
        //    try_acquire_for позволяет задать таймаут
        if (semaphore.try_acquire_for(std::chrono::milliseconds(timeout_ms))) {
            // Успех! Ресурс получен сразу
            {
                std::lock_guard<std::mutex> lock(mtx);
                std::cout << "Поток " << thread_id
                          << " (приор=" << priority << ") получил ресурс мгновенно.\n";
            }
            return;
        }

        // 2. Если ресурсов нет, становимся в очередь с приоритетом
        auto waiter = std::make_shared<WaitingThread>();
        waiter->priority = priority;

        {
            std::unique_lock<std::mutex> lock(mtx);
            waitingQueue.push({priority, waiter});
            std::cout << "Поток " << thread_id
                      << " (приор=" << priority << ") встал в очередь ожидания.\n";
        }

        // 3. Ждем, пока не будет ресурса и пока мы не станем первыми в очереди по приоритету
        auto start_time = std::chrono::steady_clock::now();
        {
            std::unique_lock<std::mutex> lock(mtx);
            // Поток ждет, пока не будет выполнено условие:
            // 1. Его флаг granted == true (его разбудил release)
            // 2. И он является вершиной очереди (его приоритет выше всех)
            bool success = waiter->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&]() {
                // Проверяем, наш ли поток первый в очереди
                if (waitingQueue.empty()) return false;
                // Сравниваем указатели: если первый в очереди - это наш waiter
                if (waitingQueue.top().second == waiter) {
                    // Наша очередь. Теперь пытаемся захватить ресурс
                    if (semaphore.try_acquire()) {
                        waiter->granted = true;
                        waitingQueue.pop(); // Убираем себя из очереди
                        return true;
                    }
                }
                return waiter->granted; // Если нас разбудили не как первого, но ресурс есть
            });

            if (success) {
                std::cout << "Поток " << thread_id
                          << " (приор=" << priority << ") получил ресурс по очереди.\n";
            } else {
                // Таймаут: убираем себя из очереди, если мы там все еще
                if (waitingQueue.top().second == waiter) {
                    waitingQueue.pop();
                } else {
                    // Нужно удалить из середины, но priority_queue не поддерживает удаление.
                    // Для простоты оставим как есть. В реальном коде нужна более сложная структура.
                }
                failed_attempts++;
                std::cout << "Поток " << thread_id
                          << " (приор=" << priority << ") не дождался ресурса (таймаут).\n";
                return;
            }
        }
    }

    void release() {
        semaphore.release(); // Освобождаем ресурс
        // Будим первый поток из очереди (если есть)
        std::unique_lock<std::mutex> lock(mtx);
        if (!waitingQueue.empty()) {
            auto top_waiter = waitingQueue.top().second;
            top_waiter->cv.notify_one();
        }
    }

    int getFailedAttempts() const { return failed_attempts.load(); }
};

// Пример использования
int main() {
    ResourcePool pool(2); // 2 ресурса

    std::vector<std::thread> threads;

    // Запускаем потоки с разным приоритетом (1 - высокий, 5 - низкий)
    threads.emplace_back([&]() { pool.acquire(5, 500); pool.release(); });
    threads.emplace_back([&]() { pool.acquire(1, 500); pool.release(); });
    threads.emplace_back([&]() { pool.acquire(3, 500); pool.release(); });
    threads.emplace_back([&]() { pool.acquire(1, 500); pool.release(); });
    threads.emplace_back([&]() { pool.acquire(5, 500); pool.release(); });

    for (auto& t : threads) t.join();

    std::cout << "Неудачных попыток: " << pool.getFailedAttempts() << std::endl;
    return 0;
}
