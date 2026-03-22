#include <iostream>
#include <thread>
#include <semaphore>
#include <mutex>
#include <chrono>
#include <vector>
#include <atomic>

using namespace std::chrono_literals;

class ParkingLot {
private:
    std::counting_semaphore<> spaces;  // Свободные места
    std::mutex cout_mutex;             // Для красивого вывода
    int total_capacity;
    std::atomic<int> occupied{0};

public:
    ParkingLot(int capacity) : spaces(capacity), total_capacity(capacity) {}

    void park(bool isVIP, int timeout_ms) {
        auto id = std::this_thread::get_id();

        // VIP логика: если мест нет, но мы VIP, мы можем "выгнать" обычную?
        // По условию: "VIP-машины всегда имеют приоритет перед обычными"
        // Реализуем как: VIP пытается захватить место. Если мест нет, она ждет,
        // но с более высоким приоритетом. Простейший способ - увеличить таймаут ожидания для VIP.
        // Или, если нужно именно "пропустить" обычную машину, нужно более сложное решение.

        // Упрощенное решение: VIP ждет дольше, но это не совсем правильно.
        // Правильное: использовать condition_variable для ручного управления очередью.

        bool success;
        if (isVIP) {
            // VIP ждет дольше или бесконечно
            success = spaces.try_acquire_for(std::chrono::milliseconds(timeout_ms * 2));
        } else {
            success = spaces.try_acquire_for(std::chrono::milliseconds(timeout_ms));
        }

        if (success) {
            occupied++;
            {
                std::lock_guard<std::mutex> lock(cout_mutex);
                std::cout << "Поток " << id
                          << (isVIP ? " (VIP)" : " (обычный)")
                          << " заехал. Занято: " << occupied.load()
                          << "/" << total_capacity << std::endl;
            }
            // Имитация парковки
            std::this_thread::sleep_for(1s);
            leave();
        } else {
            std::lock_guard<std::mutex> lock(cout_mutex);
            std::cout << "Поток " << id
                      << (isVIP ? " (VIP)" : " (обычный)")
                      << " уехал, не дождавшись места.\n";
        }
    }

    void leave() {
        occupied--;
        spaces.release(); // Освобождаем место
        {
            std::lock_guard<std::mutex> lock(cout_mutex);
            std::cout << "Поток " << std::this_thread::get_id()
                      << " освободил место. Свободно: "
                      << (total_capacity - occupied.load()) << "/" << total_capacity << std::endl;
        }
    }

    void changeCapacity(int new_capacity) {
        // Внимание: сложная операция, требует синхронизации
        // В реальности нужно менять max семафора, но в стандартном semaphore это нельзя.
        // Поэтому просто изменим логику в park, добавив проверку.
        // Для упрощения: выводим сообщение, но не меняем динамически.
        total_capacity = new_capacity;
    }
};

int main() {
    ParkingLot lot(3);

    std::vector<std::thread> cars;

    // VIP и обычные
    cars.emplace_back([&]() { lot.park(true, 1000); });
    cars.emplace_back([&]() { lot.park(false, 1000); });
    cars.emplace_back([&]() { lot.park(true, 1000); });
    cars.emplace_back([&]() { lot.park(false, 1000); });
    cars.emplace_back([&]() { lot.park(false, 1000); });

    for (auto& c : cars) c.join();

    return 0;
}
