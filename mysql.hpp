#pragma once

#include <jdbc/mysql_connection.h>
#include <jdbc/mysql_driver.h>
#include <jdbc/cppconn/driver.h>
#include <jdbc/cppconn/connection.h>
#include <jdbc/cppconn/resultset.h>
#include <jdbc/cppconn/statement.h>
#include <jdbc/cppconn/prepared_statement.h>
#include <jdbc/cppconn/exception.h>
#include <jdbc/cppconn/datatype.h>
#include <jdbc/cppconn/parameter_metadata.h>

#include "reflection.hpp"
#include "sql.hpp"


namespace mysql
{
    class MySQL
    {
        public:
            MySQL(std::string_view ip, std::string_view user, std::string_view passwd, std::string_view dbname)
                : driver_(::get_driver_instance()),
                  conn_(driver_->connect(ip.data(), user.data(), passwd.data()))
            {
                { std::unique_ptr<sql::Statement> stmt(conn_->createStatement());
                    stmt->execute(make_create_db_sql(dbname));
                }
                conn_->setSchema(dbname.data());
            }

            template <typename T, typename... Args>
            void bind_attribute(Args... args) {
                static_assert(reflection::is_reflection<T>::value, "table need to be reflected type");
                auto table_name = reflection::reflection_t<T>::name();
                utils::for_each(std::make_tuple(std::forward<Args>(args)...), [&, this](auto& t, std::size_t) {
                    for(auto name : t.fields) {
                        if(t.attr == attribute::key) {
                            this->primary_keys_[table_name].emplace(std::move(name));
                        }
                        else if(t.attr == attribute::not_null) {
                            this->not_nulls_[table_name].emplace(std::move(name));
                        }
                        else {
                            this->auto_increments_[table_name].emplace(std::move(name));
                        }
                    }
                });
            }

            template <typename T, typename... Args>
            bool create_table(Args&&... args) {
                delete_table<T>();
                constexpr auto table_name = reflection::reflection_t<T>::name();
                utils::for_each(std::make_tuple(std::forward<Args>(args)...), [&, this](auto& t, std::size_t) {
                    for(auto&& name : t.fields) {
                        if(t.attr == attribute::key) {
                            this->primary_keys_[table_name].emplace(std::move(name));
                        }
                        else if(t.attr == attribute::not_null) {
                            this->not_nulls_[table_name].emplace(std::move(name));
                        }
                        else {
                            this->auto_increments_[table_name].emplace(std::move(name));
                        }
                    }
                });
                std::unique_ptr<sql::Statement> stmt(conn_->createStatement());
                return stmt->execute(make_create_tb_sql<T>(primary_keys_, not_nulls_, auto_increments_));
            }
            template <typename T>
            bool delete_table() {
                std::unique_ptr<sql::Statement> stmt(conn_->createStatement());
                return stmt->execute(make_delete_tb_sql<T>());
            }

            template <typename... Ts, typename... Args>
            std::conditional_t<(sizeof...(Ts)==1), std::vector<Ts...>, std::vector<std::tuple<Ts...>>>
            query(std::string_view sql, Args&&... args) {
                std::unique_ptr<sql::PreparedStatement> pstmt(conn_->prepareStatement(sql.data()));
                utils::for_each(std::make_tuple(std::forward<Args>(args)...), [&pstmt](auto& item, std::size_t idx) {
                    using item_type = std::decay_t<decltype(item)>;
                    if constexpr (std::is_same_v<item_type, std::string>) {
                        pstmt->setString(idx + 1, item);
                    }
                    else if constexpr (utils::is_char_pointer_v<item_type>) {
                        pstmt->setString(idx + 1, item);
                    }
                    else if constexpr (std::is_arithmetic_v<item_type>) {
                        pstmt->setString(idx + 1, std::to_string(item));
                    }
                    else {
                        throw std::runtime_error("error");
                    }
                });
                std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
                if constexpr (sizeof...(Ts) > 1) {
                    std::vector<std::tuple<Ts...>> query_results;
                    while(res->next()) {
                        std::tuple<Ts...> t;
                        std::size_t base_idx{ 0 };
                        utils::for_each(t, [&](auto& field, std::size_t idx) {
                            idx += base_idx;
                            using field_type = std::remove_reference_t<decltype(field)>;
                            if constexpr (reflection::is_reflection<field_type>::value) {
                                reflection::for_each(field, [this, &res, idx](auto& item, std::size_t i) {
                                    this->query_impl(item, res, idx + i);
                                });
                                base_idx += reflection::reflection_t<field_type>::count() - 1;
                            }
                            else {
                                query_impl(field, res, idx);
                            }
                        });
                        query_results.emplace_back(std::move(t));
                    }
                    return query_results;
                }
                else {
                    using T = std::tuple_element_t<0, std::tuple<Ts...>>;
                    std::vector<Ts...> query_results;
                    while(res->next()) {
                        T t;
                        if constexpr (reflection::is_reflection<T>::value) {
                            reflection::for_each(t, [&](auto& field, auto idx) {
                                query_impl(field, res, idx);
                            });
                        }
                        else {
                            query_impl(t, res, 0);
                        }
                        query_results.emplace_back(std::move(t));
                    }
                    return query_results;
                }
            }

            template <typename T>
            bool insert(T&& t, null_field_set&& null_field = null_field_set{}) {
                using U = std::remove_reference_t<T>;
                static_assert(reflection::is_reflection<U>::value);
                constexpr auto field_names = reflection::get_field_names<U>();
                constexpr auto name = reflection::get_name<U>();

                std::string sql;
                utils::string_append(sql, "INSERT INTO ", name);
                std::size_t cnt = 0;
                for(auto field_name : field_names) {
                    if(null_field.count(field_name)) {
                        continue;
                    }
                    if(cnt != 0) {
                        utils::string_append(sql, ", ");
                    }
                    else {
                        utils::string_append(sql, " (");
                    }
                    utils::string_append(sql, field_name);
                    ++cnt;
                }
                if(cnt != 0) {
                    utils::string_append(sql, ") VALUES (");
                    while(cnt--) {
                        utils::string_append(sql, "?");
                        if(cnt != 0) {
                            utils::string_append(sql, ", ");
                        }
                        else {
                            utils::string_append(sql, ")");
                        }
                    }
                }
                log_info(sql);
                std::unique_ptr<sql::PreparedStatement> pstmt(conn_->prepareStatement(sql));

                std::size_t idx = 0;
                reflection::for_each(std::forward<T>(t), [&](auto& field, auto i) {
                    if(null_field.count(field_names[i])) {
                        return;
                    }
                    using field_type = std::remove_reference_t<decltype(field)>;
                    if constexpr (std::is_arithmetic_v<field_type>) {
                        pstmt->setString(idx + 1, std::to_string(field));
                    }
                    else if constexpr (std::is_same_v<std::string, field_type> || utils::is_char_pointer_v<field_type>) {
                        pstmt->setString(idx + 1, field);
                    }
                    else if constexpr (utils::is_cpp_array_v<field_type>) {
                        pstmt->setString(idx + 1, field.data());
                    }
                    else {
                        throw std::runtime_error("error");
                    }
                    ++idx;
                });
                return pstmt->execute();
            }


            template <typename T>
            bool update(T&& t, update_field_set&& update_field = update_field_set{}) {
                using U = std::remove_reference_t<T>;
                static_assert(reflection::is_reflection<U>::value);
                constexpr auto field_names = reflection::get_field_names<U>();
                constexpr auto name = reflection::get_name<U>();

                std::string sql;
                utils::string_append(sql, "UPDATE ", name);
                bool first{ true };
                for(auto& field_name : field_names) {
                    if(!update_field.count(field_name)) {
                        continue;
                    }
                    if(primary_keys_[name].count(field_name)) {
                        continue;
                    }
                    if(!first) {
                        utils::string_append(sql, ", ");
                    }
                    else {
                        utils::string_append(sql, " SET ");
                        first = false;
                    }
                    utils::string_append(sql, field_name, " = ?");
                }

                first = true;
                for(auto& field_name : field_names) {
                    if(!primary_keys_[name].count(field_name)) {
                        continue;
                    }
                    if(!first) {
                        utils::string_append(sql, " AND ");
                    }
                    else {
                        utils::string_append(sql, " WHERE ");
                        first = false;
                    }
                    utils::string_append(sql, field_name, " = ?");
                }
                log_info(sql);

                std::unique_ptr<sql::PreparedStatement> pstmt(conn_->prepareStatement(sql));
                std::size_t base_idx{ 0 };
                reflection::for_each(t, [&](auto& field, auto idx) {
                    auto field_name = field_names[idx];
                    if(!update_field.count(field_name) || primary_keys_[name].count(field_name)) {
                        return;
                    }
                    using field_type = std::remove_reference_t<decltype(field)>;
                    if constexpr (std::is_arithmetic_v<field_type>) {
                        pstmt->setString(base_idx + 1, std::to_string(field));
                    }
                    else if constexpr (std::is_same_v<std::string, field_type> || utils::is_char_pointer_v<field_type>) {
                        pstmt->setString(base_idx + 1, field);
                    }
                    else if constexpr (utils::is_cpp_array_v<field_type>) {
                        pstmt->setString(base_idx + 1, field.data());
                    }
                    else {
                        throw std::runtime_error("error");
                    }
                    ++base_idx;
                });

                reflection::for_each(std::forward<T>(t), [&](auto& field, auto idx) {
                    auto field_name = field_names[idx];
                    if(!primary_keys_[name].count(field_name)) {
                        return;
                    }
                    using field_type = std::remove_reference_t<decltype(field)>;
                    idx += base_idx;
                    if constexpr (std::is_arithmetic_v<field_type>) {
                        pstmt->setString(idx + 1, std::to_string(field));
                    }
                    else if constexpr (std::is_same_v<std::string, field_type> || utils::is_char_pointer_v<field_type>) {
                        pstmt->setString(idx + 1, field);
                    }
                    else if constexpr (utils::is_cpp_array_v<field_type>) {
                        pstmt->setString(idx + 1, field.data());
                    }
                    else {
                        throw std::runtime_error("error");
                    }
                });

                return pstmt->executeUpdate();
            }

            template <typename T, typename... Args>
            bool remove(Args&&... args) {
                using U = std::remove_reference_t<T>;
                static_assert(reflection::is_reflection<U>::value && (sizeof...(Args) > 0));
                constexpr auto field_names = reflection::get_field_names<U>();
                constexpr auto name = reflection::get_name<U>();

                std::string sql = make_remove_sql<T>(std::forward<Args>(args)...);
                std::unique_ptr<sql::Statement> stmt(conn_->createStatement());
                return stmt->execute(sql);
            }

            template <typename... Args>
            bool execute(std::string_view sql, Args&&... args) {
                std::unique_ptr<sql::PreparedStatement> pstmt(conn_->prepareStatement(sql.data()));
                utils::for_each(std::tuple(std::forward<Args>(args)...), [&](auto& field, auto idx) {
                    using field_type = std::remove_reference_t<decltype(field)>;
                    if constexpr (std::is_arithmetic_v<field_type>) {
                        pstmt->setString(idx + 1, std::to_string(field));
                    }
                    else if constexpr (std::is_same_v<std::string, field_type> || utils::is_char_pointer_v<field_type>) {
                        pstmt->setString(idx + 1, field);
                    }
                    else if constexpr (utils::is_cpp_array_v<field_type>) {
                        pstmt->setString(idx + 1, field.data());
                    }
                });
                return pstmt->execute();
            }
            void begin() {
                conn_->setAutoCommit(false);
            }
            void commit() {
                conn_->commit();
            }
            void rollback() {
                conn_->rollback();
            }
        private:
            template <typename T>
            void query_impl(T& field, std::unique_ptr<sql::ResultSet>& res, std::size_t idx) {
                using field_type = std::remove_reference_t<decltype(field)>;
                if constexpr (std::is_same_v<field_type, std::int32_t>) {
                    field = res->getInt(idx + 1);
                    log_info(field);
                }
                else if constexpr (std::is_same_v<field_type, std::uint32_t>) {
                    field = res->getUInt(idx + 1);
                }
                else if constexpr (utils::is_cpp_array_v<field_type>) {
                    std::string str = res->getString(idx + 1);
                    std::memset(&field[0], '\0', field.size());
                    std::memcpy(&field[0], &str[0], str.length());
                }
                else if constexpr (std::is_same_v<field_type, std::string>) {
                    field = res->getString(idx + 1);
                }
                else if constexpr (std::is_same_v<field_type, std::chrono::system_clock::time_point>) {
                    field = utils::string_to_time_point(res->getString(idx + 1));
                }
                else {
                    throw std::runtime_error(std::string("error field type: ") + typeid(field_type).name());
                }
            }
        private:
            sql::Driver* driver_;
            std::unique_ptr<sql::Connection> conn_;

            primary_key_map primary_keys_;
            not_null_map not_nulls_;
            auto_increment_map auto_increments_;
    };
}
