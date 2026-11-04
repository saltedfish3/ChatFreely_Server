#include "SQLTransaction.h"

Logger SQLTransaction::logger("SQLTransaction");


SQLTransaction::SQLTransaction(MYSQL* mysql)
    :mysql(mysql)
{
    mysql_autocommit(this->mysql, false);
}

SQLTransaction::~SQLTransaction()
{
    if(!this->isCommit)
	mysql_rollback(this->mysql);
    mysql_autocommit(this->mysql, true);
}

void SQLTransaction::commit()
{
    //mysql_commit(this->mysql);
    int ret = mysql_commit(this->mysql);
    if(ret != 0)
        logger.error("mysql_commit failed: errno=" + std::to_string(mysql_errno(this->mysql)) +
                     ", error=" + std::string(mysql_error(this->mysql)));
    this->isCommit = true;
}
