%{
#include "ast.h"
#include "yacc.tab.h"
#include <iostream>
#include <memory>

int yylex(YYSTYPE *yylval, YYLTYPE *yylloc);

void yyerror(YYLTYPE *locp, const char* s) {
    std::cerr << "Parser Error at line " << locp->first_line << " column " << locp->first_column << ": " << s << std::endl;
}

using namespace ast;
%}

// request a pure (reentrant) parser
%define api.pure full
// enable location in error handler
%locations
// enable verbose syntax error message
%define parse.error verbose

// keywords
%token SHOW TABLES CREATE TABLE DROP DESC INSERT INTO VALUES DELETE FROM ASC ORDER BY AS HAVING COUNT SUM MAX MIN AVG LIMIT GROUP SEMI
ON WHERE DISTINCT IN UPDATE BETWEEN SET SELECT INT CHAR FLOAT BIGINT DATETIME INDEX AND JOIN EXIT HELP TXN_BEGIN TXN_COMMIT TXN_ABORT TXN_ROLLBACK ORDER_BY ENABLE_NESTLOOP ENABLE_SORTMERGE
// non-keywords
%token LEQ NEQ GEQ T_EOF

// type-specific tokens
%token <sv_str> IDENTIFIER VALUE_STRING
%token <sv_int> VALUE_INT
%token <sv_float> VALUE_FLOAT
%token <sv_bool> VALUE_BOOL
%token <sv_datetime> VALUE_DATETIME
%token <sv_bigint> VALUE_BIGINT
%token EXPLAIN

// specify types for non-terminal symbol
%type <sv_node> stmt dbStmt ddl dml txnStmt setStmt
%type <sv_field> field
%type <sv_fields> fieldList
%type <sv_type_len> type
%type <sv_comp_op> op
%type <sv_expr> expr
%type <sv_val> value
%type <sv_vals> valueList
%type <sv_str> tbName colName Alias
%type <sv_strs> colNameList
%type <sv_col> col
%type <sv_cols> colList selector group_clause opt_group_clauses
%type <sv_set_clause> setClause
%type <sv_set_clauses> setClauses
%type <sv_cond> condition
%type <sv_conds> whereClause optWhereClause having_cond opt_having_conds
%type <sv_orderby> order_clause opt_order_clause
%type <sv_orderby_dir> opt_asc_desc
%type <sv_setKnobType> set_knob_type
%type <sv_int> opt_limit
%type <sv_jointree> tableList
%%
start:
        stmt ';'
    {
        parse_tree = $1;
        YYACCEPT;
    }
    |   EXPLAIN stmt ';'
    {
        // 标记为 explain
        if ($2) $2->is_explain = true;
        parse_tree = $2;
        YYACCEPT;
    }
    |   HELP
    {
        parse_tree = std::make_shared<Help>();
        YYACCEPT;
    }
    |   EXIT
    {
        parse_tree = nullptr;
        YYACCEPT;
    }
    |   T_EOF
    {
        parse_tree = nullptr;
        YYACCEPT;
    }
    ;

stmt:
        dbStmt
    |   ddl
    |   dml
    |   txnStmt
    |   setStmt
    ;

txnStmt:
        TXN_BEGIN
    {
        $$ = std::make_shared<TxnBegin>();
    }
    |   TXN_COMMIT
    {
        $$ = std::make_shared<TxnCommit>();
    }
    |   TXN_ABORT
    {
        $$ = std::make_shared<TxnAbort>();
    }
    | TXN_ROLLBACK
    {
        $$ = std::make_shared<TxnRollback>();
    }
    ;

dbStmt:
        SHOW TABLES
    {
        $$ = std::make_shared<ShowTables>();
    }
    ;

setStmt:
        SET set_knob_type '=' VALUE_BOOL
    {
        $$ = std::make_shared<SetStmt>($2, $4);
    }
    ;

ddl:
        CREATE TABLE tbName '(' fieldList ')'
    {
        $$ = std::make_shared<CreateTable>($3, $5);
    }
    |   DROP TABLE tbName
    {
        $$ = std::make_shared<DropTable>($3);
    }
    |   DESC tbName
    {
        $$ = std::make_shared<DescTable>($2);
    }
    |   CREATE INDEX tbName '(' colNameList ')'
    {
        $$ = std::make_shared<CreateIndex>($3, $5);
    }
    |   DROP INDEX tbName '(' colNameList ')'
    {
        $$ = std::make_shared<DropIndex>($3, $5);
    }
    |   SHOW INDEX FROM tbName
    {
        $$ = std::make_shared<ShowIndex>($4);
    }
    ;

dml:
        INSERT INTO tbName VALUES '(' valueList ')'
    {
        $$ = std::make_shared<InsertStmt>($3, $6);
    }
    |   DELETE FROM tbName optWhereClause
    {
        $$ = std::make_shared<DeleteStmt>($3, $4);
    }
    |   UPDATE tbName SET setClauses optWhereClause
    {
        $$ = std::make_shared<UpdateStmt>($2, $4, $5);
    }
    |   SELECT selector FROM tableList optWhereClause opt_group_clauses opt_having_conds opt_order_clause opt_limit
    {
        std::vector<std::string> tabs;
        std::vector<std::string> alias;
        for (int i = 0; i < (int)$4.size(); i++) {
            tabs.push_back($4[i].tab);
            alias.push_back($4[i].alias);
        }
        std::vector<std::shared_ptr<JoinExpr>> jointree;
        for (int i = 1; i < (int)$4.size(); i++) {
            auto t = std::make_shared<JoinExpr>(tabs[i - 1], tabs[i], $4[i].cond, $4[i].join_type);
            jointree.push_back(t);
        }
        $$ = std::make_shared<SelectStmt>($2, tabs, alias, jointree, $5, $6, $7, $8, $9, false);
    }
    |   SELECT DISTINCT selector FROM tableList optWhereClause opt_group_clauses opt_having_conds opt_order_clause opt_limit
    {
        std::vector<std::string> tabs;
        std::vector<std::string> alias;
        for (int i = 0; i < (int)$5.size(); i++) {
            tabs.push_back($5[i].tab);
            alias.push_back($5[i].alias);
        }
        std::vector<std::shared_ptr<JoinExpr>> jointree;
        for (int i = 1; i < (int)$5.size(); i++) {
            auto t = std::make_shared<JoinExpr>(tabs[i - 1], tabs[i], $5[i].cond, $5[i].join_type);
            jointree.push_back(t);
        }
        $$ = std::make_shared<SelectStmt>($3, tabs, alias, jointree, $6, $7, $8, $9, $10, true);
    }
    ;

fieldList:
        field
    {
        $$ = std::vector<std::shared_ptr<Field>>{$1};
    }
    |   fieldList ',' field
    {
        $$.push_back($3);
    }
    ;

colNameList:
        colName
    {
        $$ = std::vector<std::string>{$1};
    }
    | colNameList ',' colName
    {
        $$.push_back($3);
    }
    ;

field:
        colName type
    {
        $$ = std::make_shared<ColDef>($1, $2);
    }
    ;

type:
        INT
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_INT, sizeof(int));
    }
    |   CHAR '(' VALUE_INT ')'
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_STRING, $3);
    }
    |   FLOAT
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_FLOAT, sizeof(float));
    }
    |   BIGINT
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_BIGINT, sizeof(BigInt));
    }
    |   DATETIME
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_DATETIME, sizeof(DateTime));
    }
    ;

valueList:
        value
    {
        $$ = std::vector<std::shared_ptr<Value>>{$1};
    }
    |   valueList ',' value
    {
        $$.push_back($3);
    }
    ;

value:
        VALUE_INT
    {
        $$ = std::make_shared<IntLit>($1);
    }
    |   VALUE_FLOAT
    {
        $$ = std::make_shared<FloatLit>($1);
    }
    |   VALUE_STRING
    {
        $$ = std::make_shared<StringLit>($1);
    }
    |   VALUE_BOOL
    {
        $$ = std::make_shared<BoolLit>($1);
    }
    |   VALUE_BIGINT
    {
        $$ = std::make_shared<BigIntLit>($1);
    }
    |   VALUE_DATETIME
    {
        $$ = std::make_shared<DateTimeLit>($1);
    }
    ;

condition:
        col op expr
    {
        $$ = std::make_shared<BinaryExpr>($1, $2, $3);
    }
    |   col op '(' dml ')'
    {
        $$ = std::make_shared<BinaryExpr>($1, $2, $4);
    }
    |   col op '(' valueList ')'
    {
        $$ = std::make_shared<BinaryExpr>($1, $2, $4);
    }
    |   col BETWEEN value AND value
    {
        $$ = std::make_shared<BinaryExpr>($1, SV_OP_BETWEEN_AND, std::pair<std::shared_ptr<Value>, std::shared_ptr<Value>>{$3, $5});
    }
    ;

optWhereClause:
        /* epsilon */ { /* ignore*/ }
    |   WHERE whereClause
    {
        $$ = $2;
    }
    ;

whereClause:
        condition 
    {
        $$ = std::vector<std::shared_ptr<BinaryExpr>>{$1};
    }
    |   whereClause AND condition
    {
        $$.push_back($3);
    }
    ;

col:
        tbName '.' colName
    {
        $$ = std::make_shared<Col>($1, $3, "", Aggregatetype::SV_TYPE_NONE);
    }
    |   colName
    {
        $$ = std::make_shared<Col>("", $1, "", Aggregatetype::SV_TYPE_NONE);
    }
    |   tbName '.' colName AS Alias
    {
        $$ = std::make_shared<Col>($1, $3, $5, Aggregatetype::SV_TYPE_NONE);
    }
    |   colName AS Alias
    {
        $$ = std::make_shared<Col>("", $1, $3, Aggregatetype::SV_TYPE_NONE);
    }
    
    |   COUNT '(' '*' ')'
    {
        $$ = std::make_shared<Col>("", "*", "COUNT(*)", Aggregatetype::SV_TYPE_COUNT);
    }
    |   COUNT '(' '*' ')' AS Alias
    {
        $$ = std::make_shared<Col>("", "*", $6, Aggregatetype::SV_TYPE_COUNT);
    }
    |   COUNT '(' colName ')'
    {
        $$ = std::make_shared<Col>("", $3, "COUNT(" + $3 + ")", Aggregatetype::SV_TYPE_COUNT);
    }
    |   COUNT '(' colName ')' AS Alias
    {
        $$ = std::make_shared<Col>("", $3, $6, Aggregatetype::SV_TYPE_COUNT);
    }
    |   COUNT '(' tbName '.' colName ')'
    {
        $$ = std::make_shared<Col>($3, $5, "COUNT(" + $3 + '.' + $5 + ")", Aggregatetype::SV_TYPE_COUNT);
    }
    |   COUNT '(' tbName '.' colName ')' AS Alias
    {
        $$ = std::make_shared<Col>($3, $5, $8, Aggregatetype::SV_TYPE_COUNT);
    }

    |   SUM '(' colName ')'
    {
        $$ = std::make_shared<Col>("", $3, "SUM(" + $3 + ")", Aggregatetype::SV_TYPE_SUM);
    }
    |   SUM '(' colName ')' AS Alias
    {
        $$ = std::make_shared<Col>("", $3, $6, Aggregatetype::SV_TYPE_SUM);
    }
    |   SUM '(' tbName '.' colName ')'
    {
        $$ = std::make_shared<Col>($3, $5, "SUM(" + $3 + '.' + $5 + ")", Aggregatetype::SV_TYPE_SUM);
    }
    |   SUM '(' tbName '.' colName ')' AS Alias
    {
        $$ = std::make_shared<Col>($3, $5, $8, Aggregatetype::SV_TYPE_SUM);
    }

    |   MAX '(' colName ')'
    {
        $$ = std::make_shared<Col>("", $3, "MAX(" + $3 + ")", Aggregatetype::SV_TYPE_MAX);
    }
    |   MAX '(' colName ')' AS Alias
    {
        $$ = std::make_shared<Col>("", $3, $6, Aggregatetype::SV_TYPE_MAX);
    }
    |   MAX '(' tbName '.' colName ')'
    {
        $$ = std::make_shared<Col>($3, $5, "MAX(" + $3 + '.' + $5 + ")", Aggregatetype::SV_TYPE_MAX);
    }
    |   MAX '(' tbName '.' colName ')' AS Alias
    {
        $$ = std::make_shared<Col>($3, $5, $8, Aggregatetype::SV_TYPE_MAX);
    }

    |   MIN '(' colName ')'
    {
        $$ = std::make_shared<Col>("", $3, "MIN(" + $3 + ")", Aggregatetype::SV_TYPE_MIN);
    }
    |   MIN '(' colName ')' AS Alias
    {
        $$ = std::make_shared<Col>("", $3, $6, Aggregatetype::SV_TYPE_MIN);
    }
    |   MIN '(' tbName '.' colName ')'
    {
        $$ = std::make_shared<Col>($3, $5, "MIN(" + $3 + '.' + $5 + ")", Aggregatetype::SV_TYPE_MIN);
    }
    |   MIN '(' tbName '.' colName ')' AS Alias
    {
        $$ = std::make_shared<Col>($3, $5, $8, Aggregatetype::SV_TYPE_MIN);
    }

    |   AVG '(' colName ')'
    {
        $$ = std::make_shared<Col>("", $3, "AVG(" + $3 + ")", Aggregatetype::SV_TYPE_AVG);
    }
    |   AVG '(' colName ')' AS Alias
    {
        $$ = std::make_shared<Col>("", $3, $6, Aggregatetype::SV_TYPE_AVG);
    }
    |   AVG '(' tbName '.' colName ')'
    {
        $$ = std::make_shared<Col>($3, $5, "AVG(" + $3 + '.' + $5 + ")", Aggregatetype::SV_TYPE_AVG);
    }
    |   AVG '(' tbName '.' colName ')' AS Alias
    {
        $$ = std::make_shared<Col>($3, $5, $8, Aggregatetype::SV_TYPE_AVG);
    }
    
    ;

colList:
        col
    {
        $$ = std::vector<std::shared_ptr<Col>>{$1};
    }
    |   colList ',' col
    {
        $$.push_back($3);
    }
    ;

op:
        '='
    {
        $$ = SV_OP_EQ;
    }
    |   '<'
    {
        $$ = SV_OP_LT;
    }
    |   '>'
    {
        $$ = SV_OP_GT;
    }
    |   NEQ
    {
        $$ = SV_OP_NE;
    }
    |   LEQ
    {
        $$ = SV_OP_LE;
    }
    |   GEQ
    {
        $$ = SV_OP_GE;
    }
    |   IN
    {
        $$ = SV_OP_IN;
    }
    ;

expr:
        value
    {
        $$ = std::static_pointer_cast<Expr>($1);
    }
    |   col
    {
        $$ = std::static_pointer_cast<Expr>($1);
    }
    ;

setClauses:
        setClause
    {
        $$ = std::vector<std::shared_ptr<SetClause>>{$1};
    }
    |   setClauses ',' setClause
    {
        $$.push_back($3);
    }
    ;

setClause:
        colName '=' value
    {
        $$ = std::make_shared<SetClause>($1, $3);
    }
    ;

selector:
        '*'
    {
        $$ = {};
    }
    |   colList
    ;

tableList:
        tbName
    {
        $$ = std::vector<Jointree>{Jointree($1, "", JoinType::INNER_JOIN)};
    }
    | tbName AS Alias
    {
        $$ = std::vector<Jointree>{Jointree($1, $3, JoinType::INNER_JOIN)};
    }
    | tbName Alias
    {
        $$ = std::vector<Jointree>{Jointree($1, $2, JoinType::INNER_JOIN)};
    }

    |   tableList ',' tbName
    {
        $$.push_back(Jointree($3, "", JoinType::INNER_JOIN));
    }
    |   tableList ',' tbName AS Alias
    {
        $$.push_back(Jointree($3, $5, JoinType::INNER_JOIN));
    }
    |   tableList ',' tbName Alias
    {
        $$.push_back(Jointree($3, $4, JoinType::INNER_JOIN));
    }

    |   tableList JOIN tbName
    {
        $$.push_back(Jointree($3, "", JoinType::INNER_JOIN));
    }
    |   tableList JOIN tbName AS Alias
    {
        $$.push_back(Jointree($3, $5, JoinType::INNER_JOIN));
    }
    |   tableList JOIN tbName Alias
    {
        $$.push_back(Jointree($3, $4, JoinType::INNER_JOIN));
    }

    |   tableList JOIN tbName ON whereClause
    {
        $$.push_back(Jointree($3, "", JoinType::INNER_JOIN, $5));
    }
    |   tableList JOIN tbName AS Alias ON whereClause
    {
        $$.push_back(Jointree($3, $5, JoinType::INNER_JOIN, $7));
    }
    |   tableList JOIN tbName Alias ON whereClause
    {
        $$.push_back(Jointree($3, $4, JoinType::INNER_JOIN, $6));
    }

    |   tableList SEMI JOIN tbName ON whereClause
    {
        $$.push_back(Jointree($4, "", JoinType::SEMI_JOIN, $6));
    }
    |   tableList SEMI JOIN tbName AS Alias ON whereClause
    {
        $$.push_back(Jointree($4, $6, JoinType::SEMI_JOIN, $8));
    }
    |   tableList SEMI JOIN tbName Alias ON whereClause
    {
        $$.push_back(Jointree($4, $5, JoinType::SEMI_JOIN, $7));
    }
    ;

opt_having_conds :
    HAVING having_cond
    {
        $$ = $2;
    }
    |   /* epsilon */ 
    { 
        $$ = {};
    }
    ;
having_cond:
    condition
    {
        $$ = std::vector<std::shared_ptr<BinaryExpr>>{$1};
    }
    | having_cond AND condition
    {
        $$.push_back($3);
    }
    ;

opt_group_clauses:
    GROUP BY group_clause
    {
        $$ = $3;
    }
    |   /* epsilon */ 
    { 
        $$ = {};
    }
    ;
group_clause:
    col
    {
        $$ = std::vector<std::shared_ptr<Col>>{$1};
    }
    | group_clause ',' col
    {
        $$.push_back($3);
    }
    |   /* epsilon */ { /* ignore*/ }
    ;

opt_order_clause:
    ORDER BY order_clause      
    { 
        $$ = $3; 
    }
    |   /* epsilon */ 
    { 
        $$ = {};
        /* ignore*/ 
    }
    ;

order_clause:
      col opt_asc_desc 
    { 
        auto x = std::make_shared<OrderBy>($1, $2);
        $$ = std::vector<std::shared_ptr<OrderBy>>{x};
    }
    | order_clause ',' col opt_asc_desc 
    { 
        auto x = std::make_shared<OrderBy>($3, $4);
        $$.push_back(x);
    }
    ;   

opt_asc_desc:
    ASC          { $$ = OrderBy_ASC;     }
    |  DESC      { $$ = OrderBy_DESC;    }
    |       { $$ = OrderBy_DEFAULT; }
    ;    

set_knob_type:
    ENABLE_NESTLOOP { $$ = EnableNestLoop; }
    |   ENABLE_SORTMERGE { $$ = EnableSortMerge; }
    ;

opt_limit:
    LIMIT VALUE_INT
    {
        $$ = $2;
    }
    | /* epsilon */ 
    {
        $$ = -1;
    }
    ;

tbName: IDENTIFIER;

colName: IDENTIFIER;

Alias: IDENTIFIER;

%%
