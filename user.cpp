class User
    {
    public:
        User(uint32_t id, const std::string& username)
            : id_(id), username_(username), is_online_(false)
        {
        }

        uint32_t GetId() const { return id_; }
        uint64_t GetPassword() const {return password_;}
        const std::string& GetUsername() const { return username_; }
        bool IsOnline() const { return is_online_; }
        void SetOnline(bool online) { is_online_ = online; }
        
        void SetSession(std::shared_ptr<ChatSession> session) { session_ = session; }
        std::weak_ptr<ChatSession> GetSession() const { return session_; }

    private:
        uint32_t id_;
        uint64_t password_;
        std::string username_;
        bool is_online_;
        std::weak_ptr<ChatSession> session_;
    };



    class UserManager
    {
    public:
        UserManager() : next_user_id_(1) {}

        std::shared_ptr<User> CreateUser(const std::string& username)
        {
            std::lock_guard<std::mutex> lock(users_mutex_);
            
            // 중복 사용자명 확인
            for (const auto& pair : users_)
            {
                if (pair.second->GetUsername() == username){
                    return nullptr; // 이미 존재하는 사용자명
                }
                    
            }

            uint32_t user_id = next_user_id_++;
            auto user = std::make_shared<User>(user_id, username);
            users_[user_id] = user;
            
            std::cout << "User created: " << username << " (ID: " << user_id << ")" << std::endl;
            return user;
        }

        bool GetAutority(uint32_t user_id, uint64_t password)
        {
            std::lock_guard<std::mutex> lock(users_mutex_);
            
            
        }
        std::shared_ptr<User> GetUser(uint32_t user_id)
        {
            std::lock_guard<std::mutex> lock(users_mutex_);
            auto it = users_.find(user_id);
            return (it != users_.end()) ? it->second : nullptr;
        }

        std::shared_ptr<User> GetUserByName(const std::string& username)
        {
            std::lock_guard<std::mutex> lock(users_mutex_);
            for (const auto& pair : users_)
            {
                if (pair.second->GetUsername() == username)
                    return pair.second;
            }
            return nullptr;
        }

        bool RemoveUser(uint32_t user_id)
        {
            std::lock_guard<std::mutex> lock(users_mutex_);
            auto it = users_.find(user_id);
            if (it != users_.end())
            {
                std::cout << "User removed: " << it->second->GetUsername() 
                        << " (ID: " << user_id << ")" << std::endl;
                users_.erase(it);
                return true;
            }
            return false;
        }

        std::vector<std::shared_ptr<User>> GetOnlineUsers()
        {
            std::lock_guard<std::mutex> lock(users_mutex_);
            std::vector<std::shared_ptr<User>> online_users;
            
            for (const auto& pair : users_)
            {
                if (pair.second->IsOnline())
                {
                    online_users.push_back(pair.second);
                }
            }
            
            return online_users;
        }

        size_t GetUserCount() const
        {
            std::lock_guard<std::mutex> lock(users_mutex_);
            return users_.size();
        }

    private:
        std::unordered_map<uint32_t, std::shared_ptr<User>> users_;
        mutable std::mutex users_mutex_;
        std::atomic<uint32_t> next_user_id_;
    };
