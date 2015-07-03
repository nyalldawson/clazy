#include "foreacher.h"
#include "Utils.h"

#include <clang/AST/AST.h>

using namespace clang;
using namespace std;

const std::map<std::string, std::vector<std::string> > & detachingMethodsMap()
{
    static std::map<std::string, std::vector<std::string> > methodsMap;
    if (methodsMap.empty()) {
        methodsMap["QList"] = {"first", "last", "begin", "end", "front", "back"};
        methodsMap["QVector"] = {"first", "last", "begin", "end", "front", "back", "data", "fill"};
        methodsMap["QMap"] = {"begin", "end", "first", "find", "last", "lowerBound", "upperBound"};
        methodsMap["QHash"] = {"begin", "end", "find"};
        methodsMap["QLinkedList"] = {"first", "last", "begin", "end", "front", "back"};
        methodsMap["QSet"] = {"begin", "end", "find"};
        methodsMap["QStack"] = {"top"};
        methodsMap["QQueue"] = {"head"};
        methodsMap["QMultiMap"] = methodsMap["QMap"];
        methodsMap["QMultiHash"] = methodsMap["QHash"];
        methodsMap["QString"] = {"begin", "end", "data"};
        methodsMap["QByteArray"] = {"data"};
        methodsMap["QImage"] = {"bits", "scanLine"};
    }

    return methodsMap;
}

Foreacher::Foreacher(clang::CompilerInstance &ci)
    : CheckBase(ci)
{

}

void Foreacher::VisitStmt(clang::Stmt *stmt)
{
    auto forStm = dyn_cast<ForStmt>(stmt);
    if (forStm != nullptr) {
        m_lastForStmt = forStm;
        return;
    }

    if (m_lastForStmt == nullptr)
        return;

    CXXConstructExpr *constructExpr = dyn_cast<CXXConstructExpr>(stmt);
    if (constructExpr == nullptr)
        return;

    CXXConstructorDecl *constructorDecl = constructExpr->getConstructor();
    if (constructorDecl == nullptr || constructorDecl->getNameAsString() != "QForeachContainer")
        return;

    vector<DeclRefExpr*> declRefExprs;
    Utils::getChilds2<DeclRefExpr>(constructExpr, declRefExprs);
    if (declRefExprs.empty())
        return;

    // Get the container value declaration
    DeclRefExpr *declRefExpr = declRefExprs.front();
    ValueDecl *valueDecl = dyn_cast<ValueDecl>(declRefExpr->getDecl());
    if (valueDecl == nullptr)
        return;

    checkBigTypeMissingRef();

    // const containers are fine
    if (valueDecl->getType().isConstQualified())
        return;

    // Now look inside the for statement for detachments
    if (containsDetachments(m_lastForStmt, valueDecl)) {
        emitWarning(stmt->getLocStart(), "foreach container detached [-Wmore-warnings-foreach-detachment]");
    }
}


std::string Foreacher::name() const
{
    return "foreacher";
}

void Foreacher::checkBigTypeMissingRef()
{
    // Get the inner forstm
    vector<ForStmt*> forStatements;
    Utils::getChilds2<ForStmt>(m_lastForStmt->getBody(), forStatements);
    if (forStatements.empty())
        return;

    // Get the variable declaration (lhs of foreach)
    vector<DeclStmt*> varDecls;
    Utils::getChilds2<DeclStmt>(forStatements.at(0), varDecls);
    if (varDecls.empty())
        return;

    Decl *decl = varDecls.at(0)->getSingleDecl();
    if (decl == nullptr)
        return;
    VarDecl *varDecl = dyn_cast<VarDecl>(decl);
    if (varDecl == nullptr)
        return;

    QualType qt = varDecl->getType();
    const Type *t = qt.getTypePtrOrNull();
    if (t == nullptr || t->isLValueReferenceType()) // it's a reference, we're good
        return;

    const int size_of_T = m_ci.getASTContext().getTypeSize(varDecl->getType()) / 8;
    const bool isLarge = size_of_T > 16;
    CXXRecordDecl *recordDecl = t->getAsCXXRecordDecl();

    // other ctors are fine, just check copy ctor and dtor
    const bool isUserNonTrivial = recordDecl && (recordDecl->hasUserDeclaredCopyConstructor() || recordDecl->hasUserDeclaredDestructor());
    // const bool isLiteral = t->isLiteralType(m_ci.getASTContext());

    std::string error;
    if (isLarge) {
        error = "Missing reference in foreach with sizeof(T) = ";
        error += std::to_string(size_of_T) + " bytes";
    } else if (isUserNonTrivial) {
        error = "Missing reference in foreach with non trivial type";
    }

    if (error.empty()) // No warning
        return;

    error += " [-Wmore-warnings-missing-ref-foreach]";

    // If it's const, then it's definitely missing &. But if it's not const, there might be a non-const member call, which we should allow
    if (qt.isConstQualified()) {
        emitWarning(varDecl->getLocStart(), error.c_str());
        return;
    }


    // Lets look for it.
    std::vector<CXXMemberCallExpr*> memberCalls;
    Utils::getChilds2<CXXMemberCallExpr>(forStatements.at(0), memberCalls);

    for (auto it = memberCalls.cbegin(), end = memberCalls.cend(); it != end; ++it) {
        CXXMemberCallExpr *memberCall = *it;
        CXXMethodDecl *methodDecl = memberCall->getMethodDecl();
        if (methodDecl == nullptr || methodDecl->isConst())
            continue;

        ValueDecl *valueDecl = Utils::valueDeclForMemberCall(*it);
        if (valueDecl == nullptr)
            continue;

        if (valueDecl == varDecl) // We found it, a non-const member call is ok, no ref needed
            return;
    }

    emitWarning(varDecl->getLocStart(), error.c_str());
}

bool Foreacher::containsDetachments(Stmt *stm, clang::ValueDecl *containerValueDecl)
{
    if (stm == nullptr)
        return false;

    auto memberExpr = dyn_cast<MemberExpr>(stm);
    if (memberExpr != nullptr) {
        ValueDecl *valDecl = memberExpr->getMemberDecl();
        if (valDecl != nullptr && valDecl->isCXXClassMember()) {
            DeclContext *declContext = valDecl->getDeclContext();
            auto recordDecl = dyn_cast<CXXRecordDecl>(declContext);
            if (recordDecl != nullptr) {
                const std::string className = recordDecl->getQualifiedNameAsString();
                if (detachingMethodsMap().find(className) != detachingMethodsMap().end()) {
                    const std::string functionName = valDecl->getNameAsString();
                    const auto &allowedFunctions = detachingMethodsMap().at(className);
                    if (std::find(allowedFunctions.cbegin(), allowedFunctions.cend(), functionName) != allowedFunctions.cend()) {
                        Expr *expr = memberExpr->getBase();
                        if (expr && llvm::isa<DeclRefExpr>(expr)) {
                            if (dyn_cast<DeclRefExpr>(expr)->getDecl() == containerValueDecl) { // Finally, check if this non-const member call is on the same container we're iterating
                                return true;
                            }
                        }
                    }
                }
            }
        }
    }

    auto it = stm->child_begin();
    auto end = stm->child_end();
    for (; it != end; ++it) {
        if (containsDetachments(*it, containerValueDecl))
            return true;
    }

    return false;
}