#pragma once

#include <thread>
#include <vector>
#include <atomic>
#include <functional>
#include <queue>
#include <mutex>
#include <type_traits>
#include <condition_variable>


namespace utils {
    class cancelation_exception : public std::exception
    {
        using std::exception::exception;
        const char* what() const noexcept override {
            return "Task cancelled";
        }
    };

    class recursion_found_exception : public std::exception
    {
        using std::exception::exception;
        const char* what() const noexcept override {
            return "Recursuon found";
        }
    };

    class shutdown_exception : public std::exception
    {
        using std::exception::exception;
        const char* what() const noexcept override {
            return "Thread pool is shutdown";
        }
    };

    enum state {
        DONE, CANCELED, RUNNING, FAILED, WAIT
    };

    template <class ResultType>
    struct Data
    {
        Data(const Data&) = delete;
        Data(Data&&) = delete;
        Data& operator=(Data) = delete;

        template <class F, class... Args>
        explicit Data(F&& f, Args&&... args) : state_(WAIT) {
            func_ = std::function<ResultType()>([f_ = std::forward<F>(f),
                args_ = std::make_tuple(std::forward<Args>(args)...)] () mutable {
                return std::apply(f_, std::move(args_));
            });
        }

        void run() {
            {
                std::lock_guard<std::mutex> state_lock(state_mutex);
                if (state_ != WAIT)
                    return;
            }
            
            {
                std::lock_guard<std::mutex> state_lock(state_mutex);
                state_ = RUNNING;
            }
            try {
                result_ = func_();
            }
            catch (...) {
                {
                    std::lock_guard<std::mutex> state_lock(state_mutex);
                    state_ = FAILED;
                }
                exception = std::current_exception();
                std::lock_guard<std::mutex> lock(mutex);
                cv.notify_all();
                return;
            }
            {
                std::lock_guard<std::mutex> state_lock(state_mutex);
                state_ = DONE;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                cv.notify_all();
            }

            std::lock_guard<std::mutex> lock(waiting_mutex);
            for (size_t i = 0; i < waiting_tasks.size(); ++i) {
                try {
                    waiting_tasks[i](result_);
                }
                catch (std::exception&) {
                    ;
                }
            }
        }

        std::function<ResultType()> func_;
        ResultType result_;
        std::atomic<state> state_;
        std::exception_ptr exception;
        std::vector<std::thread::id> threads_id;
        std::vector<std::function<void(ResultType)>> waiting_tasks;

        mutable std::mutex mutex;
        mutable std::mutex waiting_mutex;
        mutable std::mutex state_mutex;
        mutable std::condition_variable cv;
    };

    template <>
    struct Data<void>
    {
        Data(const Data&) = delete;
        Data(Data&&) = delete;
        Data& operator=(Data) = delete;

        template <class F, class... Args>
        Data(F&& f, Args&&... args) : state_(WAIT) {
            func_ = std::function<void()>([f_ = std::forward<F>(f),
                args_ = std::make_tuple(std::forward<Args>(args)...)] () mutable {
                return std::apply(f_, std::move(args_));
            });
        }

        void run() {
            {
                std::lock_guard<std::mutex> state_lock(state_mutex);
                if (state_ != WAIT)
                    return;
            }
            
            {
                std::lock_guard<std::mutex> state_lock(state_mutex);
                state_ = RUNNING;
            }
            try {
                func_();
            }
            catch (...) {
                {
                    std::lock_guard<std::mutex> state_lock(state_mutex);
                    state_ = FAILED;
                }
                exception = std::current_exception();
                std::lock_guard<std::mutex> lock(mutex);
                cv.notify_all();
                return;
            }
            {
                std::lock_guard<std::mutex> state_lock(state_mutex);
                state_ = DONE;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                cv.notify_all();
            }
             
            std::lock_guard<std::mutex> lock(waiting_mutex);
            for (size_t i = 0; i < waiting_tasks.size(); ++i ) {
                try {
                    waiting_tasks[i]();
                }
                catch (std::exception&) {
                    ;
                }
            }
        }

        std::function<void()> func_;
        std::atomic<state> state_;
        std::exception_ptr exception;
        std::vector<std::thread::id> threads_id;
        std::vector<std::function<void()>> waiting_tasks;

        mutable std::mutex mutex;
        mutable std::mutex waiting_mutex;
        mutable std::mutex state_mutex;
        mutable std::condition_variable cv;
    };


    class TaskBase
    {
    public:
        virtual ~TaskBase() = default;
        virtual void run() const = 0;
        void operator()() {
            run();
        }
        virtual void set_id(const std::vector<std::thread::id>& threads_id) = 0;
    };

    template <class ResultType>
    class task : public TaskBase {
        std::shared_ptr<Data<ResultType>> task_;

        void run() const override {
            task_->run();
        }
    public:
        task() noexcept : task_(nullptr) {}
        task(std::shared_ptr<Data<ResultType>>& data) noexcept : task_(data) {}

        task(const task&) = delete;
        task& operator=(task&) = delete;

        task(task&& other) noexcept = default;
        task& operator=(task&& other) noexcept = default;

        void set_id(const std::vector<std::thread::id>& threads_id) override {
            task_->threads_id = threads_id;
        }

        ResultType get() const {
            if (task_->state_ == CANCELED)
                throw cancelation_exception();
            if (task_->state_ == WAIT 
                && std::find(task_->threads_id.begin(), task_->threads_id.end(), std::this_thread::get_id()) != task_->threads_id.end())
                run();
            {
                std::unique_lock<std::mutex> lock(task_->mutex);
                task_->cv.wait(lock, [this]() {return task_->state_ == DONE || task_->state_ == CANCELED || task_->state_ == FAILED; });
            }
            if (task_->state_ == FAILED)
                std::rethrow_exception(task_->exception);
            return task_->result_;
        }

        void invoke_on_completion(std::function<void(ResultType)> f) {
            std::lock_guard<std::mutex> lock(task_->waiting_mutex);
            if (task_->state_ == CANCELED || task_->state_ == FAILED)
                return;
            if (task_->state_ == DONE) {
                try {
                    f(task_->result_);
                }
                catch (std::exception&) {
                    ;
                }
            }
            else {
                task_->waiting_tasks.push_back(f);
            }
        }

        void cancel() noexcept {
            std::lock_guard<std::mutex> state_lock(task_->state_mutex);
            if (task_->state_ != RUNNING) {
                task_->state_ = CANCELED;
            }
        }

        bool is_running() const {
            return task_->state_ == RUNNING;
        }

        bool is_done() const {
            return task_->state_ == DONE;
        }

        bool is_canceled() const {
            return task_->state_ == CANCELED;
        }

        ~task() {
            if (task_ && task_->state_ == WAIT)
                cancel();
        }
    };

    template <>
    class task<void> : public TaskBase {
        std::shared_ptr<Data<void>> task_;

        void run() const override {
            task_->run();
        }
    public:
        task(std::shared_ptr<Data<void>> data) : task_(data) {}

        task(const task&) = delete;
        task& operator=(task&) = delete;

        task(task&& other) noexcept = default;
        task& operator=(task&& other) noexcept = default;

        void set_id(const std::vector<std::thread::id>& threads_id) override {
            task_->threads_id = threads_id;
        }

        void get() const {
            if (task_->state_ == CANCELED)
                throw cancelation_exception();
            if (task_->state_ == WAIT
                && std::find(task_->threads_id.begin(), task_->threads_id.end(), std::this_thread::get_id()) != task_->threads_id.end())
                run();
            {
                std::unique_lock<std::mutex> lock(task_->mutex);
                task_->cv.wait(lock, [this]() {return task_->state_ == DONE || task_->state_ == CANCELED || task_->state_ == FAILED; });
            }
            if (task_->state_ == FAILED)
                std::rethrow_exception(task_->exception);
        }

        void invoke_on_completion(std::function<void()> f) {
            std::lock_guard<std::mutex> lock(task_->waiting_mutex);
            if (task_->state_ == CANCELED || task_->state_ == FAILED)
                return;
            if (task_->state_ == DONE) {
                try {
                    f();
                }
                catch (std::exception&) {
                    ;
                }
            }
            else {
                task_->waiting_tasks.push_back(f);
            }
        }

        void cancel() noexcept {
            std::lock_guard<std::mutex> state_lock(task_->state_mutex);
            if (task_->state_ != RUNNING) {
                task_->state_ = CANCELED;
            }
        }

        bool is_running() const {
            return task_->state_ == RUNNING;
        }

        bool is_done() const {
            return task_->state_ == DONE;
        }

        bool is_canceled() const {
            return task_->state_ == CANCELED;
        }

        ~task() {
            if (task_ && task_->state_ == WAIT)
                cancel();
        }
    };

    
    class thread_pool {
        std::vector<std::thread> threads_;
        std::queue<std::shared_ptr<TaskBase>> tasks_;

        std::mutex mutex;
        std::condition_variable cv;

        std::atomic<bool> isDone_;

        std::vector<std::thread::id> threads_id;

    public:
        template<typename R>
        using task_t = task<R>;

        explicit thread_pool(size_t count = std::thread::hardware_concurrency()) : isDone_(false) {
            threads_.resize(count);
            for (size_t i = 0; i < count; ++i) {
                set_thread(i);
            }
            for (size_t i = 0; i < count; ++i) {
                threads_id.push_back(threads_[i].get_id());
            }
        }

        template<class F, class... Args>
        void enqueue(F&& f, Args&&... args) {
            if (isDone_)
                throw shutdown_exception();
            using R = typename std::result_of<decltype(f)(Args...)>::type;
            auto tmp = std::make_shared<task<R>>(std::make_shared<Data<R>>(std::forward<F>(f), std::forward<Args>(args)...));
            tmp->set_id(threads_id);
            std::lock_guard<std::mutex> lock(mutex);
            tasks_.push(std::move(tmp));
            cv.notify_one();
        }

        void add_task(std::shared_ptr<TaskBase>&& task) {
            task->set_id(threads_id);
            std::lock_guard<std::mutex> lock(mutex);
            tasks_.push(std::move(task));
            cv.notify_one();
        }

        ~thread_pool() {
            isDone_ = true;
            {
                std::lock_guard<std::mutex> lock(mutex);
                cv.notify_all();
            }
        
            for (size_t i = 0; i < threads_.size(); ++i) {
                if (threads_[i].joinable())
                    threads_[i].join();
            }
            threads_.clear();
        }


        size_t threads_count() const {
            return threads_.size();
        }


        size_t remaining_tasks() const {
            return tasks_.size();
        }

        template <class F, class... Args>
        auto submit(F&& f, Args&&... args) {
            if (isDone_)
                throw shutdown_exception();
            using R = typename std::result_of<decltype(f)(Args...)>::type;
            auto tmp = std::make_shared<Data<R>>(std::forward<F>(f), std::forward<Args>(args)...);
            add_task(std::make_shared<task<R>>(tmp));
            return task<R>(tmp);
        }


    private:
        thread_pool(const thread_pool&) = delete;
        thread_pool(thread_pool&&) = delete;
        thread_pool& operator=(const thread_pool&) = delete;
        thread_pool& operator=(thread_pool&&) = delete;

        void set_thread(size_t i) {
            auto f = [this]() {
                bool isPop;
                std::shared_ptr<TaskBase> task_;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    isPop = !tasks_.empty();
                    if (isPop) {
                        task_ = tasks_.front();
                        tasks_.pop();
                    }
                }
                while (true) {
                    while (isPop) {
                        try {
                            (*task_)();
                        }
                        catch (std::exception&) {
                            ;
                        }
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            isPop = !tasks_.empty();
                            if (isPop) {
                                task_ = tasks_.front();
                                tasks_.pop();
                            }
                        }
                    }
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait(lock, [this, &isPop]() { isPop = !this->tasks_.empty(); return isPop || isDone_; });
                    if (isPop) {
                        task_ = tasks_.front();
                        tasks_.pop();
                    }
                    if (isDone_ && !isPop)
                        return;
                }
            };
            threads_[i] = std::thread(f);
        }
    };
}

