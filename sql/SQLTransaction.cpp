#include "SQLTransaction.h"

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
    mysql_commit(this->mysql);
    this->isCommit = true;
}
