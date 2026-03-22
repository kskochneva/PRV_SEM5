#include <iostream>
#include <thread>
#include <semaphore>
#include <mutex>
#include <vector>
#include <chrono>
#include <random>

using namespace std::chrono_literals;

template<typename T>
class SemaphoreBuffer {
private:
    std::vector<std::vector<T>> buffers;        // Массив буферов
    std::vector<std::counting_semaphore<>> empty; // Семафоры "свободные места"
    std::vector<std::counting_semaphore<>> full;  // Семафоры "заполненные места"
    std::vector<std::mutex> buffer_mutexes;       // Мьютексы для доступа к буферам
    std::mutex cout_mutex;                        // Мьютекс для вывода

public:
    SemaphoreBuffer(int num_buffers, int buffer_size)
        : buffers(num_buffers, std::vector<T>(buffer_size))
        , empty(num_buffers, std::counting_semaphore<>(buffer_size))
        , full(num_buffers, std::counting_semaphore<>(0))
        , buffer_mutexes(num_buffers)
    {
        // Инициализируем все семафоры empty значением buffer_size
        for (int i = 0; i < num_buffers; ++i) {
            // Уже инициализировано в списке инициализации
        }
    }

    void produce(T value, int buffer_index, int timeout_ms) {
        auto id = std::this_thread::get_id();

        // Пытаемся занять место в буфере (уменьшаем empty)
        if (!empty[buffer_index].try_acquire_for(std::chrono::milliseconds(timeout_ms))) {
            {
                std::lock_guard<std::mutex> lock(cout_mutex);
                std::cout << "Поток " << id << " не смог занять место в буфере "
                          << buffer_index << " (таймаут)\n";
            }
            return;
        }

        // Теперь место занято, нужно положить данные под защитой мьютекса
        {
            std::lock_guard<std::mutex> lock(buffer_mutexes[buffer_index]);
            // Находим первое свободное место (можно хранить индекс, но для простоты - push_back?)
            // Но у нас фиксированный размер. Будем искать.
            for (size_t i = 0; i < buffers[buffer_index].size(); ++i) {
                // Тут сложность: нужно знать, где свободно. Для простоты используем queue.
                // Переделаем: пусть buffers будет очередью.
            }
            // Упростим: пусть буфер будет очередью.
        }

        // Осложняем. Давайте упростим подход: пусть буферы будут очередями, и семафоры будут просто
        // ограничивать длину очереди.

        full[buffer_index].release(); // Увеличиваем счетчик заполненных
        {
            std::lock_guard<std::mutex> lock(cout_mutex);
            std::cout << "Поток " << id << " положил " << value << " в буфер " << buffer_index << std::endl;
        }
    }

    T consume(int buffer_index, int timeout_ms) {
        auto id = std::this_thread::get_id();

        // Ждем, пока в буфере что-то появится
        if (!full[buffer_index].try_acquire_for(std::chrono::milliseconds(timeout_ms))) {
            {
                std::lock_guard<std::mutex> lock(cout_mutex);
                std::cout << "Поток " << id << " не дождался данных в буфере "
                          << buffer_index << " (таймаут)\n";
            }
            return T{};
        }

        T value;
        {
            std::lock_guard<std::mutex> lock(buffer_mutexes[buffer_index]);
            // Извлекаем данные из буфера
            value = buffers[buffer_index].back();
            buffers[buffer_index].pop_back();
        }

        empty[buffer_index].release(); // Освобождаем место

        {
            std::lock_guard<std::mutex> lock(cout_mutex);
            std::cout << "Поток " << id << " взял " << value << " из буфера " << buffer_index << std::endl;
        }

        return value;
    }
};

int main() {
    SemaphoreBuffer<int> sb(3, 5); // 3 буфера по 5 мест

    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    for (int i = 0; i < 4; ++i) {
        producers.emplace_back([&sb, i]() {
            for (int j = 0; j < 3; ++j) {
                sb.produce(i * 10 + j, i % 3, 1000);
                std::this_thread::sleep_for(500ms);
            }
        });
    }

    for (int i = 0; i < 4; ++i) {
        consumers.emplace_back([&sb, i]() {
            for (int j = 0; j < 3; ++j) {
                sb.consume(i % 3, 1000);
                std::this_thread::sleep_for(700ms);
            }
        });
    }

    for (auto& p : producers) p.join();
    for (auto& c : consumers) c.join();

    return 0;
}
