//
//  ClassWithSomething.m
//  Sample
//
//

#import "MixinWithSomething.h"

@interface MixinWithSomething ()

@property NSString* name;

@end

@implementation MixinWithSomething

- (instancetype)initWithName:(NSString *)name
{
    if (self = [super init]) {
        self.name = name;
    }
    
    return self;
}

- (NSString *)invertName
{
    return self.name;
}

- (NSString *)concatenameWithPrefix:(NSString *)prefix
                              sufix:(NSString *)suffix
{
    return self.name;
}

@end
