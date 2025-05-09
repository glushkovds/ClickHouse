#include <Common/typeid_cast.h>
#include <Interpreters/CrossToInnerJoinVisitor.h>
#include <Interpreters/DatabaseAndTableWithAlias.h>
#include <Interpreters/IdentifierSemantic.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTSubquery.h>
#include <Parsers/ASTTablesInSelectQuery.h>
#include <Parsers/ExpressionListParsers.h>
#include <Parsers/parseQuery.h>

#include <Common/logger_useful.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int INCORRECT_QUERY;
    extern const int LOGICAL_ERROR;
    extern const int NOT_IMPLEMENTED;
}

namespace
{

struct JoinedElement
{
    explicit JoinedElement(const ASTTablesInSelectQueryElement & table_element)
        : element(table_element)
    {
        if (element.table_join)
        {
            join = element.table_join->as<ASTTableJoin>();
            original_kind = join->kind;
        }
    }

    void checkTableName(const DatabaseAndTableWithAlias & table, const String & current_database) const
    {
        if (!element.table_expression)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Not a table expression in JOIN (ARRAY JOIN?)");

        ASTTableExpression * table_expression = element.table_expression->as<ASTTableExpression>();
        if (!table_expression)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Wrong table expression in JOIN");

        if (!table.same(DatabaseAndTableWithAlias(*table_expression, current_database)))
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Inconsistent table names");
    }

    void rewriteCommaToCross()
    {
        if (join && join->kind == JoinKind::Comma)
            join->kind = JoinKind::Cross;
    }

    JoinKind getOriginalKind() const { return original_kind; }

    bool rewriteCrossToInner(ASTPtr on_expression)
    {
        if (join->kind != JoinKind::Cross)
            return false;

        join->kind = JoinKind::Inner;
        join->strictness = JoinStrictness::All;

        join->on_expression = on_expression;
        join->children = {join->on_expression};
        return true;
    }

    ASTPtr arrayJoin() const { return element.array_join; }
    const ASTTableJoin * tableJoin() const { return join; }

    bool canAttachOnExpression() const { return join && !join->on_expression; }
    bool hasUsing() const { return join && join->using_expression_list; }

private:
    const ASTTablesInSelectQueryElement & element;
    ASTTableJoin * join = nullptr;

    JoinKind original_kind;
};

bool isAllowedToRewriteCrossJoin(const ASTPtr & node, const Aliases & aliases)
{
    if (node->as<ASTFunction>())
    {
        auto idents = IdentifiersCollector::collect(node);
        for (const auto * ident : idents)
        {
            if (ident->isShort() && aliases.contains(ident->shortName()))
                return false;
        }
        return true;
    }
    return node->as<ASTIdentifier>() || node->as<ASTLiteral>();
}

/// Return mapping table_no -> expression with expression that can be moved into JOIN ON section
std::map<size_t, std::vector<ASTPtr>> moveExpressionToJoinOn(
    const ASTPtr & ast,
    const std::vector<JoinedElement> & joined_tables,
    const std::vector<TableWithColumnNamesAndTypes> & tables,
    const Aliases & aliases)
{
    std::map<size_t, std::vector<ASTPtr>> asts_to_join_on;
    for (const auto & node : splitConjunctionsAst(ast))
    {
        if (const auto * func = node->as<ASTFunction>(); func && func->name == "equals")
        {
            if (!func->arguments || func->arguments->children.size() != 2)
                return {};

            /// Check if the identifiers are from different joined tables.
            /// If it's a self joint, tables should have aliases.
            auto left_table_pos = IdentifierSemantic::getIdentsMembership(func->arguments->children[0], tables, aliases);
            auto right_table_pos = IdentifierSemantic::getIdentsMembership(func->arguments->children[1], tables, aliases);

            /// Identifiers from different table move to JOIN ON
            if (left_table_pos && right_table_pos && *left_table_pos != *right_table_pos)
            {
                size_t table_pos = std::max(*left_table_pos, *right_table_pos);
                if (joined_tables[table_pos].canAttachOnExpression())
                    asts_to_join_on[table_pos].push_back(node);
                else
                    return {};
            }
        }

        if (!isAllowedToRewriteCrossJoin(node, aliases))
            return {};
    }
    return asts_to_join_on;
}

ASTPtr makeOnExpression(const std::vector<ASTPtr> & expressions)
{
    if (expressions.size() == 1)
        return expressions[0]->clone();

    ASTs arguments;
    arguments.reserve(expressions.size());
    for (const auto & ast : expressions)
        arguments.emplace_back(ast->clone());

    return makeASTFunction("and", std::move(arguments));
}

std::vector<JoinedElement> getTables(const ASTSelectQuery & select)
{
    if (!select.tables())
        return {};

    const auto * tables = select.tables()->as<ASTTablesInSelectQuery>();
    if (!tables)
        return {};

    size_t num_tables = tables->children.size();
    if (num_tables < 2)
        return {};

    std::vector<JoinedElement> joined_tables;
    joined_tables.reserve(num_tables);
    bool has_using = false;

    for (const auto & child : tables->children)
    {
        const auto * table_element = child->as<ASTTablesInSelectQueryElement>();
        if (!table_element)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "TablesInSelectQueryElement expected");

        JoinedElement & t = joined_tables.emplace_back(*table_element);
        t.rewriteCommaToCross();

        if (t.arrayJoin())
            return {};

        if (t.hasUsing())
        {
            if (has_using)
                throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Multiple USING statements are not supported");
            has_using = true;
        }

        if (const auto * join = t.tableJoin(); join && isCrossOrComma(join->kind))
        {
            if (!join->children.empty())
                throw Exception(
                    ErrorCodes::LOGICAL_ERROR, "CROSS JOIN has {} expressions: [{}, ...]",
                    join->children.size(), join->children[0]->formatWithSecretsOneLine());
        }
    }

    return joined_tables;
}

}


bool CrossToInnerJoinMatcher::needChildVisit(ASTPtr & node, const ASTPtr &)
{
    return !node->as<ASTSubquery>();
}

void CrossToInnerJoinMatcher::visit(ASTPtr & ast, Data & data)
{
    if (auto * t = ast->as<ASTSelectQuery>())
        visit(*t, ast, data);
}

void CrossToInnerJoinMatcher::visit(ASTSelectQuery & select, ASTPtr &, Data & data)
{
    std::vector<JoinedElement> joined_tables = getTables(select);
    if (joined_tables.empty())
        return;

    /// Check if joined_tables are consistent with known tables_with_columns
    {
        if (joined_tables.size() != data.tables_with_columns.size())
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                            "Inconsistent number of tables: {} != {}",
                            joined_tables.size(), data.tables_with_columns.size());

        for (size_t i = 0; i < joined_tables.size(); ++i)
            joined_tables[i].checkTableName(data.tables_with_columns[i].table, data.current_database);
    }

    /// CROSS to INNER
    if (data.cross_to_inner_join_rewrite && select.where())
    {
        auto asts_to_join_on = moveExpressionToJoinOn(select.where(), joined_tables, data.tables_with_columns, data.aliases);
        for (size_t i = 1; i < joined_tables.size(); ++i)
        {
            auto & joined = joined_tables[i];
            if (joined.tableJoin()->kind != JoinKind::Cross)
                continue;

            String query_before = joined.tableJoin()->formatWithSecretsOneLine();
            bool rewritten = false;
            const auto & expr_it = asts_to_join_on.find(i);
            if (expr_it != asts_to_join_on.end())
            {
                ASTPtr on_expr = makeOnExpression(expr_it->second);
                if (rewritten = joined.rewriteCrossToInner(on_expr); rewritten)
                {
                    LOG_DEBUG(getLogger("CrossToInnerJoin"), "Rewritten '{}' to '{}'", query_before, joined.tableJoin()->formatForLogging());
                }
            }

            if (joined.getOriginalKind() == JoinKind::Comma &&
                data.cross_to_inner_join_rewrite > 1 &&
                !rewritten)
            {
                throw Exception(
                    ErrorCodes::INCORRECT_QUERY,
                    "Failed to rewrite comma join to INNER. "
                    "Please, try to simplify WHERE section "
                    "or set the setting `cross_to_inner_join_rewrite` to 1 to allow slow CROSS JOIN for this case "
                    "(cannot rewrite '{} WHERE {}' to INNER JOIN)",
                    query_before, select.where()->formatForErrorMessage());
            }
        }
    }
}

}
