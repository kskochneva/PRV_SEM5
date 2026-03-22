#include <iostream>
#include <thread>
#include <queue>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>
#include <random>
#include <numeric>

using namespace std::chrono_literals;

// Структура задачи
struct Task {
    int id;
    int required_slots;    // Сколько ресурсов нужно
    int duration_ms;       // Время выполнения
    int priority;          // Приоритет (1 - самый высокий)
    std::chrono::steady_clock::time_point submittedTime;
    
    bool operator<(const Task& other) const {
        return priority > other.priority;  // Меньше число = выше приоритет
    }
    
    void execute() {
        std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
    }
};

class TaskScheduler {
private:
    std::priority_queue<Task> taskQueue;           // Очередь задач
    std::mutex queueMutex;
    std::condition_variable cv;
    
    std::counting_semaphore<> resourceSemaphore;   // Ограничитель ресурсов
    std::atomic<int> completedTasks{0};
    std::atomic<long long> totalWaitTime{0};       // Общее время ожидания
    
    int totalResources;                            // Всего ресурсов в системе
    std::mutex coutMutex;
    
public:
    TaskScheduler(int total_resources) 
        : resourceSemaphore(total_resources), totalResources(total_resources) {}
    
    // Добавление задачи в очередь
    void submit(Task task) {
        task.submittedTime = std::chrono::steady_clock::now();
        
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            taskQueue.push(task);
            
            std::lock_guard<std::mutex> coutLock(coutMutex);
            std::cout << "[SUBMIT] Задача #" << task.id 
                      << " (приор=" << task.priority 
                      << ", требует=" << task.required_slots 
                      << " ресурсов) добавлена\n";
        }
        cv.notify_one();
    }
    
    // Поток-исполнитель
    void worker(int workerId) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> yieldChance(1, 100);
        
        while (true) {
            Task task;
            bool hasTask = false;
            
            // Извлекаем задачу из очереди
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                cv.wait(lock, [this] { return !taskQueue.empty(); });
                task = taskQueue.top();
                taskQueue.pop();
                hasTask = true;
            }
            
            if (!hasTask) continue;
            
            // Вычисляем время ожидания
            auto waitTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - task.submittedTime
            ).count();
            totalWaitTime += waitTime;
            
            {
                std::lock_guard<std::mutex> coutLock(coutMutex);
                std::cout << "[WORKER " << workerId << "] Поток " 
                          << std::this_thread::get_id() 
                          << " начал выполнение задачи #" << task.id 
                          << " (приор=" << task.priority 
                          << ", ресурсов=" << task.required_slots 
                          << ", ожидал=" << waitTime << "мс)\n";
            }
            
            // Захватываем необходимые ресурсы
            for (int i = 0; i < task.required_slots; ++i) {
                resourceSemaphore.acquire();
            }
            
            {
                std::lock_guard<std::mutex> coutLock(coutMutex);
                std::cout << "[WORKER " << workerId << "] Задача #" << task.id 
                          << " захватила " << task.required_slots << " ресурсов\n";
            }
            
            // Выполняем задачу с периодической уступкой CPU
            auto startTime = std::chrono::steady_clock::now();
            while (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime).count() < task.duration_ms) {
                
                // Имитация работы
                std::this_thread::sleep_for(100ms);
                
                // Периодически уступаем CPU
                if (yieldChance(gen) > 95) {
                    std::this_thread::yield();
                }
            }
            
            // Освобождаем ресурсы
            for (int i = 0; i < task.required_slots; ++i) {
                resourceSemaphore.release();
            }
            
            completedTasks++;
            
            {
                std::lock_guard<std::mutex> coutLock(coutMutex);
                std::cout << "[WORKER " << workerId << "] Задача #" << task.id 
                          << " завершена (освобождено " << task.required_slots << " ресурсов)\n";
            }
            
            // Уступаем CPU после завершения задачи
            std::this_thread::yield();
        }
    }
    
    int getCompletedTasks() const { return completedTasks.load(); }
    long long getTotalWaitTime() const { return totalWaitTime.load(); }
    double getAverageWaitTime() const {
        int completed = completedTasks.load();
        return completed > 0 ? (double)totalWaitTime.load() / completed : 0.0;
    }
};

// Функция для генерации случайных задач
void generateTasks(TaskScheduler& scheduler, int numTasks) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> priorityDist(1, 5);
    std::uniform_int_distribution<> slotsDist(1, 3);
    std::uniform_int_distribution<> durationDist(500, 3000);
    
    for (int i = 1; i <= numTasks; ++i) {
        Task task;
        task.id = i;
        task.priority = priorityDist(gen);
        task.required_slots = slotsDist(gen);
        task.duration_ms = durationDist(gen);
        
        scheduler.submit(task);
        
        // Задержка между добавлением задач
        std::this_thread::sleep_for(200ms);
    }
}

int main() {
    const int TOTAL_RESOURCES = 5;  // Всего 5 единиц ресурсов
    const int NUM_WORKERS = 3;      // 3 потока-исполнителя
    const int NUM_TASKS = 10;       // 10 задач
    
    TaskScheduler scheduler(TOTAL_RESOURCES);
    std::vector<std::thread> workers;
    
    std::cout << "=== ПЛАНИРОВЩИК ЗАДАЧ ===\n";
    std::cout << "Всего ресурсов: " << TOTAL_RESOURCES << "\n";
    std::cout << "Потоков-исполнителей: " << NUM_WORKERS << "\n\n";
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Запускаем потоки-исполнители
    for (int i = 1; i <= NUM_WORKERS; ++i) {
        workers.emplace_back(&TaskScheduler::worker, &scheduler, i);
        // Открепляем потоки, чтобы они работали в фоне
        workers.back().detach();
    }
    
    // Генерируем задачи
    generateTasks(scheduler, NUM_TASKS);
    
    // Ждем завершения всех задач
    std::this_thread::sleep_for(10s);
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto totalTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    
    std::cout << "\n========== СТАТИСТИКА ==========\n";
    std::cout << "Выполнено задач: " << scheduler.getCompletedTasks() << "/" << NUM_TASKS << "\n";
    std::cout << "Среднее время ожидания: " << scheduler.getAverageWaitTime() << " мс\n";
    std::cout << "Общее время работы: " << totalTime.count() << " мс\n";
    std::cout << "================================\n";
    
    return 0;
}
